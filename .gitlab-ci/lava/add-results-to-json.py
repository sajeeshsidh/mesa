#!/usr/bin/env python3
#
# Copyright (C) 2023 Collabora Limited
# Authors:
#     Detlev Casanova <detlev.casanova@collabora.com>
#
# SPDX-License-Identifier: MIT

"""Read deqp-runner results CSV file and add them to the json results file"""

import json
import pathlib
from dataclasses import dataclass, fields

import csv
import fire

@dataclass
class PathResolver:
    def __post_init__(self):
        for field in fields(self):
            value = getattr(self, field.name)
            if not value:
                continue
            if field.type == pathlib.Path:
                value = pathlib.Path(value)
                setattr(self, field.name, value.resolve())

@dataclass
class CSVToJSon(PathResolver):
    csv_file: pathlib.Path = None
    json_file: pathlib.Path = None

    def convert(self) -> None:
        """
        Convert the CSV file to json and add it to the json file.
        It will convert CSV lines as:
            "test_name,Pass,1.5"
        to
            {
              "test": "test_name",
              "result": "Pass",
              "duration": "1.5"
            }
        """
        converted: list[dict[str, str]] = []

        with open(self.csv_file, newline='') as csvfile:
            resreader = list(csv.reader(csvfile, delimiter=','))
            for result in resreader:
                converted.append({"test": result[0], "result": result[1], "duration": result[2]})

        with open(self.json_file) as out_file:
            structural_log = json.load(out_file)

        structural_log["deqp_runner_results"] = converted

        with open(self.json_file, 'w') as out_file:
            json.dump(structural_log, out_file, indent = 4, sort_keys=True)

if __name__ == "__main__":
    fire.Fire(CSVToJSon)
