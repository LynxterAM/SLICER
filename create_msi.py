#!/usr/bin/env python3
"""Create the Windows MSI from an extracted build/package directory."""

import argparse
import os
import re
import shutil
import subprocess
import uuid
from pathlib import Path


def parse_cmake_variables(path):
    pattern = re.compile(r'^\s*set\(\s*([A-Za-z_][A-Za-z0-9_]*)\s+"([^"]*)"\s*\)')
    variables = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            match = pattern.match(line)
            if match:
                variables[match.group(1)] = match.group(2)
    return variables


def msi_version(version):
    """Convert the release version to the four-part MSI version format."""
    if not version:
        raise ValueError("No MSI version was provided and none was found in version.inc.")
    version = version.strip()
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)\.(\d+)(?:[+-].*)?", version)
    if not match:
        raise ValueError(
            f"Invalid MSI version '{version}'. Expected four numeric parts, "
            "for example 1.0.0.0."
        )
    parts = [int(part) for part in match.groups()]
    if any(part > 255 for part in parts):
        raise ValueError("Each MSI version part must be between 0 and 255.")
    return ".".join(str(part) for part in parts)


def write_license_rtf(source, destination):
    with source.open(encoding="utf-8", errors="replace") as stream:
        content = stream.read()
    content = content.replace("\r\n", "\n").replace("\r", "\n")
    content = content.replace("\\", "\\\\").replace("{", "\\{").replace("}", "\\}")
    content = content.replace("\n", r"\par ")
    rtf = r"{\rtf1\ansi\deff0 {\fonttbl {\f0 Times New Roman;}}{\f0\fs20 " + content + "}}"
    destination.write_text(rtf, encoding="ascii", errors="replace", newline="")


def find_wix_tools(wix_bin):
    names = ("heat.exe", "candle.exe", "light.exe")
    search_dirs = []
    if wix_bin:
        search_dirs.append(wix_bin)
    for variable in ("WIX", "WIX_TOOLSET"):
        value = os.environ.get(variable)
        if value:
            root = Path(value)
            search_dirs.extend((root, root / "bin"))
    for root in (
        Path(os.environ.get("ProgramFiles(x86)", "")),
        Path(os.environ.get("ProgramFiles", "")),
    ):
        if str(root) != ".":
            search_dirs.extend(root.glob("WiX Toolset v*/bin"))

    tools = {}
    missing = []
    for name in names:
        path = shutil.which(name)
        if path is None:
            for directory in search_dirs:
                candidate = directory / name
                if candidate.is_file():
                    path = str(candidate)
                    break
        if path is None:
            missing.append(name)
        else:
            tools[name] = Path(path)
    if missing:
        raise FileNotFoundError(
            "WiX tools not found: " + ", ".join(missing) + ". "
            "Install WiX Toolset with 'choco install wixtoolset -y' or use --wix-bin."
        )
    return tools


def run_tool(tool, arguments, cwd):
    command = [str(tool)] + [str(argument) for argument in arguments]
    print("+ " + subprocess.list2cmdline(command))
    subprocess.run(command, cwd=cwd, check=True)


def render_installer_template(template, destination, variables):
    content = template.read_text(encoding="utf-8")
    for name, value in variables.items():
        content = content.replace(f"@{name}@", value)
    unresolved = sorted(set(re.findall(r"@[A-Za-z_][A-Za-z0-9_]*@", content)))
    if unresolved:
        raise ValueError("Unresolved installer variables: " + ", ".join(unresolved))
    destination.write_text(content, encoding="utf-8", newline="")


def build_msi(args):
    script_root = Path(__file__).resolve().parent
    package_dir = args.package_dir.resolve()
    if not package_dir.is_dir():
        raise FileNotFoundError(f"Package directory not found: {package_dir}")

    version_file = (args.version_file or script_root / "version.inc").resolve()
    template = (args.template or script_root / "src/platform/msw/winInstaller.wxs.in").resolve()
    license_file = (args.license_file or script_root / "LICENSE").resolve()
    variables = parse_cmake_variables(version_file)
    required = (
        "SLIC3R_APP_NAME",
        "SLIC3R_APP_KEY",
        "SLIC3R_APP_CMD",
        "SLIC3R_APP_PROG_ID",
        "SLIC3R_GUID",
    )
    missing = [name for name in required if name not in variables]
    if missing:
        raise ValueError(f"Missing variables in {version_file}: {', '.join(missing)}")

    version = args.installer_version
    if not version:
        version = variables.get("SLIC3R_RC_VERSION_DOTS")
        if not version and variables.get("SLIC3R_RC_VERSION"):
            version = variables["SLIC3R_RC_VERSION"].replace(",", ".")
        if version:
            version = re.sub(r"\.\d+(?:[+-].*)?$", ".0", version)
    version = msi_version(version)

    output = args.output
    if output is None:
        output = package_dir.parent / f"{variables['SLIC3R_APP_KEY']}_{version}.msi"
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    executable = package_dir / f"{variables['SLIC3R_APP_CMD']}.exe"
    if not executable.is_file():
        raise FileNotFoundError(f"Main executable not found in package: {executable}")
    if not template.is_file():
        raise FileNotFoundError(f"WiX template not found: {template}")
    if not license_file.is_file() and not (package_dir / "LICENSE.rtf").is_file():
        raise FileNotFoundError(f"License file not found: {license_file}")

    tools = find_wix_tools(args.wix_bin.resolve() if args.wix_bin else None)
    work_dir = args.work_dir.resolve() if args.work_dir else package_dir.parent
    if not work_dir.is_dir():
        raise FileNotFoundError(f"MSI work directory not found: {work_dir}")
    run_id = uuid.uuid4().hex
    installer_wxs = work_dir / f"lynxter-winInstaller-{run_id}.wxs"
    files_wxs = work_dir / f"lynxter-files-{run_id}.wxs"
    installer_wixobj = work_dir / f"lynxter-winInstaller-{run_id}.wixobj"
    files_wixobj = work_dir / f"lynxter-files-{run_id}.wixobj"
    pdb_file = work_dir / f"lynxter-{run_id}.wixpdb"
    try:
        license_rtf = package_dir / "LICENSE.rtf"
        if license_file.is_file():
            write_license_rtf(license_file, license_rtf)

        render_installer_template(template, installer_wxs, variables)

        run_tool(
            tools["heat.exe"],
            [
                "dir",
                package_dir,
                "-dr",
                "INSTALLFOLDER",
                "-cg",
                "Package",
                "-gg",
                "-sreg",
                "-srd",
                "-var",
                "var.SourceDir",
                "-out",
                files_wxs,
                "-sfrag",
            ],
            work_dir,
        )
        run_tool(
            tools["candle.exe"],
            [
                f"-dSourceDir={package_dir}",
                f"-dInstallerVersion={version}",
                f"-dReleaseSuffix={args.release_suffix}",
                installer_wxs,
                files_wxs,
            ],
            work_dir,
        )
        run_tool(
            tools["light.exe"],
            [
                "-b",
                package_dir,
                "-ext",
                "WixUIExtension",
                installer_wixobj,
                files_wixobj,
                "-pdbout",
                pdb_file,
                "-o",
                output,
                "-sw1076",
            ],
            work_dir,
        )
    finally:
        for generated_file in (installer_wxs, files_wxs, installer_wixobj, files_wixobj, pdb_file):
            try:
                generated_file.unlink()
            except FileNotFoundError:
                pass

    print(f"MSI created: {output}")


def main():
    parser = argparse.ArgumentParser(
        description="Create a WiX MSI from an extracted Windows build/package directory."
    )
    parser.add_argument(
        "package_dir",
        type=Path,
        help="Directory containing the extracted build/package files.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Output MSI path. Defaults to the parent of package_dir.",
    )
    parser.add_argument(
        "--version",
        dest="installer_version",
        help="Four-part MSI version, for example 1.0.0.0.",
    )
    parser.add_argument(
        "--release-suffix",
        default="",
        help="Optional suffix passed to the WiX Product name.",
    )
    parser.add_argument("--wix-bin", type=Path, help="Directory containing heat.exe, candle.exe and light.exe.")
    parser.add_argument("--work-dir", type=Path, help="Existing directory for temporary WiX files.")
    parser.add_argument("--version-file", type=Path, help="Path to version.inc.")
    parser.add_argument("--template", type=Path, help="Path to winInstaller.wxs.in.")
    parser.add_argument("--license-file", type=Path, help="Path to the plain-text LICENSE file.")
    args = parser.parse_args()
    try:
        build_msi(args)
    except (FileNotFoundError, PermissionError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
