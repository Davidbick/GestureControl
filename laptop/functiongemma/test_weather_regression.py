"""Opt-in live regression for the unchanged official base-model weather path."""
import os
import subprocess
import sys
import unittest
from pathlib import Path


@unittest.skipUnless(os.environ.get('FUNCTIONGEMMA_LIVE_TESTS') == '1',
                     'Set FUNCTIONGEMMA_LIVE_TESTS=1 for cached-model inference')
class WeatherRegression(unittest.TestCase):
    def test_official_weather_sequence(self):
        result = subprocess.run(
            [sys.executable, '-u', str(Path(__file__).with_name('transformers_weather.py'))],
            capture_output=True, text=True, timeout=180,
            env={**os.environ, 'HF_HUB_OFFLINE':'1'},
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('WEATHER: PASS', result.stdout)
        self.assertIn('"name": "get_current_weather"', result.stdout)
        self.assertIn('"location": "Tokyo, Japan"', result.stdout)


if __name__ == '__main__':
    unittest.main()
