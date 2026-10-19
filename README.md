# ps5-linux-patches

## Compilation

```bash
git clone https://github.com/ps5-linux/ps5-linux-patches
git clone https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git
cd linux
git checkout "tags/v$(grep -m1 "^# Linux/" ../ps5-linux-patches/.config | awk '{print $3}')"
git apply ../ps5-linux-patches/linux.patch
cp ../ps5-linux-patches/.config .config
make -j$(nproc)
```

## Installation

In the same `linux` folder after compilation, do:

```bash
sudo make modules_install && sudo make install
```

## GitHub Actions kernel releases

The existing `deploy` workflow builds kernel packages on pushes to `main`. To
select a package format or patches ref manually, run **Build Kernel** from the
repository's **Actions** tab. Formats are `deb`, `arch` (`.pkg.tar.zst`), `rpm`,
and `all` (the default). `all` compiles the kernel once, then packages all three
formats. These are kernel-only builds: no disk images or R2 credentials are needed.

The repository needs an online self-hosted runner with labels `self-hosted`,
`Linux`, and `X64`, Git, Bash, a working Docker daemon accessible to the runner
user, and passwordless `sudo -n` for removing root-owned files in the build's
`image` directory. Kernel jobs are serialized per repository without cancelling
an active build.

The workflow checks out `<repository-owner>/ps5-linux-image` at `main`, so that
owner's builder fork must contain the required builder changes. Patches come from
the repository running the workflow, at the supplied `patches_ref` or the event's
commit SHA when omitted. The compiler cache persists at
`$HOME/.cache/ps5-linux-image/ccache`, outside the cleaned builder checkout.
For the kernel source fetch only, command-scoped Git configuration redirects the
stable kernel.org URL to the official stable maintainer's
[`gregkh/linux` mirror](https://github.com/gregkh/linux); it does not change global
Git configuration or the selected kernel tag.

Successful runs upload the selected packages and a relative-filename
`SHA256SUMS` file to the run's **Artifacts** section (retained for seven days).
Non-PR runs also publish them under **Releases**, using
`kernel-<kernel-version>-<short-patches-SHA>` tags. Release notes link the exact
patches and builder commits and include the checksums. The `.deb` generates its
initramfs when installed on the target system using its available initramfs
generator; the build does not ship a pre-generated target initramfs.

## TODO

- amdgpu smu driver to show correct gpu frequency and temperature
- hdmi converter improvments: hdr, rgb range, 120hz

## Bugs

- screen save does not work properly
- hdmi audio output does not work on some monitors
- hdmi 1440p and 2160p video output does not work on some monitors
- monitor swap is not supported
- hdmi cec is not supported
