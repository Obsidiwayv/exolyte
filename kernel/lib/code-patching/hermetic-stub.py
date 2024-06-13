#!/usr/bin/env fuchsia-vendored-python
# Copyright 2021 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

import argparse
import json
import os
import string

from datetime import datetime

STUB_CONTENTS = string.Template(
    """// Copyright $year The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

// GENERATED BY //zircon/kernel/lib/code-patching/hermetic-stub.py.
// DO NOT EDIT.

#include <lib/arch/asm.h>
#include <lib/code-patching/asm.h>
#include <$header>  // Defines $case_id.

.text

.function $function_name, global
  .code_patching.blob $size, $case_id
.end_function
"""
)

ALIAS = string.Template(
    """
.weak $alias
.hidden $alias
$alias = $function_name
"""
)


def main():
    parser = argparse.ArgumentParser(
        description="Creates an assembly file with a single stub functon to be later patched"
    )
    parser.add_argument("--name", help="The name of the stub function")
    parser.add_argument(
        "--header", help="A header containing patch case ID constants"
    )
    parser.add_argument(
        "--metadata-file",
        metavar="FILE",
        help="a JSON file of hermetic patch alternative metadata",
    )
    parser.add_argument(
        "--aliases",
        type=str,
        action="append",
        help="Symbol names to alias to the function as",
        default=[],
    )
    parser.add_argument(
        "--depfile", metavar="FILE", help="A dependencies file to write to"
    )
    parser.add_argument(
        "--outfile", metavar="FILE", help="The resulting stub file"
    )
    args = parser.parse_args()

    with open(args.metadata_file, "r") as metadata_file:
        metadata = json.load(metadata_file)

    max_size = 0
    alternatives = []
    for entry in metadata:
        alternative = entry["path"]
        alternatives.append(alternative)
        max_size = max(max_size, os.path.getsize(alternative))

    with open(args.depfile, "w") as depfile:
        depfile.write(
            "%s: %s\n" % (args.outfile, " ".join(sorted(alternatives)))
        )

    with open(args.outfile, "w") as stub:
        stub.write(
            STUB_CONTENTS.substitute(
                year=datetime.today().year,
                header=args.header,
                function_name=args.name,
                # Per the expectations of code_patching_hermetic_stub()'s
                # `case_id_header` parameter.
                case_id="CASE_ID_%s" % args.name.upper(),
                size=max_size,
            )
        )
        for alias in args.aliases:
            stub.write(ALIAS.substitute(alias=alias, function_name=args.name))


if __name__ == "__main__":
    main()
