"""Prepare shared WinUI build inputs for Meson and Visual Studio."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request
import zipfile
import winreg


# SDK packages are build inputs only; application libraries still use vcpkg.
PACKAGES = {
    "microsoft.web.webview2": "1.0.3179.45",
    "microsoft.windows.cppwinrt": "2.0.240111.5",
    "microsoft.windowsappsdk.foundation": "1.8.251220000",
    "microsoft.windowsappsdk.interactiveexperiences": "1.8.251217001",
    "microsoft.windowsappsdk.winui": "1.8.251222000",
}


def package(name, version, output, cache):
    """Reuse the NuGet cache, or download the exact version into the build tree."""
    cached = cache / name / version
    if (cached / f"{name}.nuspec").is_file():
        return cached
    target = output / "packages" / name / version
    if (target / f"{name}.nuspec").is_file():
        return target

    # A failed download/extraction is not marked complete and can be retried.
    archive = output / f"{name}.{version}.nupkg"
    url = f"https://api.nuget.org/v3-flatcontainer/{name}/{version}/{archive.name}"
    urllib.request.urlretrieve(url, archive)
    with zipfile.ZipFile(archive) as source:
        source.extractall(target)
    archive.unlink()
    return target


def main():
    # The build system supplies the target architecture independently of the host.
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arch", choices=["x64", "x86", "arm64"], required=True)
    parser.add_argument("--sdk", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cache = Path(os.environ.get("NUGET_PACKAGES", Path.home() / ".nuget/packages"))

    # Prefer the SDK selected by the compiler environment, then the installed SDK.
    if args.sdk is None:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SOFTWARE\Microsoft\Windows Kits\Installed Roots") as key:
            kits = Path(winreg.QueryValueEx(key, "KitsRoot10")[0])
        versions = list((kits / "UnionMetadata").glob("10.*/Windows.winmd"))
        args.sdk = max(versions, key=lambda path: tuple(map(int, path.parent.name.split('.'))))

    # Keep the native WinRT projection reproducible across developer machines.
    packages = {name: package(name, version, output, cache)
                for name, version in PACKAGES.items()}
    cppwinrt = packages["microsoft.windows.cppwinrt"] / "bin/cppwinrt.exe"
    foundation = packages["microsoft.windowsappsdk.foundation"]
    interaction = packages["microsoft.windowsappsdk.interactiveexperiences"]
    winui = packages["microsoft.windowsappsdk.winui"]
    projection = output / "include"
    command = [str(cppwinrt), "-out", str(projection), "-in", str(args.sdk)]
    command += ["-in", str(winui / "metadata")]
    command += ["-in", str(interaction / "metadata/10.0.17763.0")]
    command += ["-in", str(foundation / "metadata")]
    command += ["-in", str(packages["microsoft.web.webview2"] / "lib/Microsoft.Web.WebView2.Core.winmd")]
    subprocess.run(command, check=True)

    # Copy SDK interop headers and embed the architecture-matched bootstrapper.
    shutil.copytree(winui / "include", projection, dirs_exist_ok=True)
    bootstrap = foundation / f"runtimes/win-{args.arch}/native/Microsoft.WindowsAppRuntime.Bootstrap.dll"
    embedded = output / "Microsoft.WindowsAppRuntime.Bootstrap.dll"
    shutil.copyfile(bootstrap, embedded)
    (output / "bootstrap.rc").write_text(
        f'201 RCDATA "{embedded.as_posix()}"\n', encoding="utf-8")
    (output / "sdk-versions.txt").write_text(
        "\n".join(f"{name}={version}" for name, version in PACKAGES.items()),
        encoding="utf-8")


if __name__ == "__main__":
    main()
