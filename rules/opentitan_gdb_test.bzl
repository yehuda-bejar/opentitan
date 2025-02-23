# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("@bazel_skylib//lib:shell.bzl", "shell")
load("@lowrisc_opentitan//rules:rv.bzl", "rv_rule")

def _opentitan_gdb_fpga_cw310_test(ctx):
    # Write the GDB script to disk and load it with GDB's `--command` argument.
    # This enables us to separate lines with whitespace, whereas if we piped the
    # string into GDB's stdin, each newline would cause it to repeat the
    # previous command.
    gdb_script_file = ctx.actions.declare_file("{}.gdb".format(ctx.label.name))
    test_script_file = ctx.actions.declare_file("{}.sh".format(ctx.label.name))

    # This dummy script exists because test rules are a kind of executable rule,
    # and executable rules *must* produce an output file.
    test_script = """#!/usr/bin/env bash
    set -ex
    {coordinator_script} \\
      --rom-kind={rom_kind} \\
      --openocd-earlgrey-config={openocd_earlgrey_config} \\
      --exit-success-pattern={exit_success_pattern} \\
      --bitstream-path={bitstream_path} \\
      --gdb-script-path={gdb_script_path} \\
      --opentitantool-path={opentitantool_path}
    """.format(
        coordinator_script = shell.quote(ctx.executable._coordinator.short_path),
        rom_kind = shell.quote(ctx.attr.rom_kind),
        openocd_earlgrey_config = shell.quote(ctx.file._openocd_earlgrey_config.path),
        exit_success_pattern = shell.quote(ctx.attr.exit_success_pattern),
        bitstream_path = shell.quote(ctx.file.rom_bitstream.short_path),
        gdb_script_path = shell.quote(gdb_script_file.short_path),
        opentitantool_path = shell.quote(ctx.file._opentitantool.short_path),
    )

    ctx.actions.write(output = gdb_script_file, content = ctx.attr.gdb_script)
    ctx.actions.write(output = test_script_file, content = test_script)

    # Construct a dict that we can pass to `ctx.runfiles()`, mapping symlink
    # names to real file paths.
    gdb_script_symlinks_flipped = {}
    for label in ctx.attr.gdb_script_symlinks:
        label_files = label.files.to_list()
        if len(label_files) != 1:
            fail("gdb_script_symlinks labels must have exactly one file, but", label, "has these files:", label_files)
        gdb_script_symlinks_flipped[ctx.attr.gdb_script_symlinks[label]] = label_files[0]

    gdb_script_runfiles = ctx.runfiles(
        symlinks = gdb_script_symlinks_flipped,
        files = gdb_script_symlinks_flipped.values(),
    )

    test_script_runfiles = ctx.runfiles(
        files = [
            ctx.file._openocd_earlgrey_config,
            ctx.file._opentitantool,
            ctx.file.rom_bitstream,
            gdb_script_file,
        ],
    ).merge(ctx.attr._coordinator.data_runfiles)

    return [DefaultInfo(
        executable = test_script_file,
        runfiles = test_script_runfiles.merge(gdb_script_runfiles),
    )]

# Orchestrate opentitantool, OpenOCD, and GDB to load the given program into
# SRAM and execute it in-place. This rule assumes that a CW310 FPGA and an
# ARM-USB-TINY-H JTAG debugger are attached to the host.
opentitan_gdb_fpga_cw310_test = rv_rule(
    implementation = _opentitan_gdb_fpga_cw310_test,
    attrs = {
        "exit_success_pattern": attr.string(mandatory = True),
        "gdb_script": attr.string(mandatory = True),
        "gdb_script_symlinks": attr.label_keyed_string_dict(allow_files = True),
        "rom_bitstream": attr.label(
            mandatory = True,
            allow_single_file = True,
        ),
        "rom_kind": attr.string(mandatory = True),
        "_coordinator": attr.label(
            default = "//rules/scripts:gdb_test_coordinator",
            cfg = "exec",
            executable = True,
        ),
        "_opentitantool": attr.label(
            default = "//sw/host/opentitantool",
            allow_single_file = True,
            cfg = "exec",
        ),
        "_openocd_earlgrey_config": attr.label(
            default = "//util/openocd/target:lowrisc-earlgrey.cfg",
            allow_single_file = True,
        ),
    },
    test = True,
)
