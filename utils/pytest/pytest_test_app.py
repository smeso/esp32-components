from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@idf_parametrize("target", ["supported_targets"], indirect=["target"])
def test_unit_test(dut: Dut) -> None:
    dut.expect_exact("[doctest] Status: SUCCESS!", timeout=120)
