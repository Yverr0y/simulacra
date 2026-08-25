import os, sys, unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
import capture_profile as C


def adv(addr, atype, n, sig="01", company=None):
    return [{"ts": float(i), "addr": addr, "atype": atype, "company": company,
             "ad_sig": sig, "rssi": -60} for i in range(n)]


class AtypeIsDeviceWeighted(unittest.TestCase):
    """profile['atype'] must count DEVICES, not adverts.

    It is compared against synth_distributions()['atype'], where one row is one device. Address type
    is fixed by the address's top two bits, so every advert from an address carries the same atype
    and advert-weighting measures only how chatty that device is. RPA phones advertise far faster
    than static beacons: on the 2026-08-25 ambient baseline the per-advert count read 0.022 static
    where the device mix was 0.250, an 11x distortion on a mixed basis.
    """

    def test_one_chatty_device_cannot_dominate(self):
        # Two devices, one static and one RPA, but the RPA emits 1000x more adverts.
        prof = C.build_profile(adv("aa", "static", 1) + adv("bb", "rpa", 1000))
        self.assertAlmostEqual(prof["atype"]["static"], 0.5, places=6,
                               msg="atype is advert-weighted: a chatty RPA drowned out the static "
                                   "device. It must be device-weighted like vendor/ad_sig.")
        self.assertAlmostEqual(prof["atype"]["rpa"], 0.5, places=6)

    def test_fractions_sum_to_one_over_devices(self):
        prof = C.build_profile(adv("aa", "static", 3) + adv("bb", "rpa", 7) + adv("cc", "public", 2))
        self.assertEqual(prof["n_addrs"], 3)
        self.assertAlmostEqual(sum(prof["atype"].values()), 1.0, places=6)
        for k in ("static", "rpa", "public"):
            self.assertAlmostEqual(prof["atype"][k], 1 / 3, places=6)

    def test_atype_uses_same_denominator_as_ad_sig(self):
        """Both are device-weighted, so a single-device-per-type capture must agree exactly."""
        prof = C.build_profile(adv("aa", "static", 50, sig="01") + adv("bb", "rpa", 5, sig="ff"))
        self.assertAlmostEqual(prof["atype"]["static"], prof["ad_sig"]["01"], places=6)


if __name__ == "__main__":
    unittest.main()
