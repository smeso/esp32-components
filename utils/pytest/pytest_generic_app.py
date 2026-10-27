"""Generic QEMU test: check that an ESP-IDF firmware prints to stdout.

The expected output can be customized with the EXPECT_PATTERN and
EXPECT_TIMEOUT environment variables. By default any non-empty output
line is accepted.
"""

import os
import re

from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@idf_parametrize("target", ["supported_targets"], indirect=["target"])
def test_stdout(dut: Dut) -> None:
    pattern = os.environ.get("EXPECT_PATTERN", "main_task: Calling app_main")
    timeout = float(os.environ.get("EXPECT_TIMEOUT", "10"))
    dut.expect(re.compile(pattern), timeout=timeout)
