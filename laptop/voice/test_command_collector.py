import unittest
from command_collector import CommandCollector


class CollectorTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.results = []
        self.collector = CommandCollector(
            self.results.append, clock=lambda: self.now)

    def advance(self, seconds):
        self.now += seconds
        self.collector.poll()

    def test_multi_line_explicit_finish(self):
        self.collector.on_line('Hi, Gemma, I want to play Minecraft.')
        self.collector.on_line('Make my upper swipe move forward.')
        self.advance(2)
        self.collector.on_line('Make my lower swipe move backward.')
        self.advance(3)
        self.collector.on_line('And make my left swipe move left. Gemma do that.')
        self.assertEqual(self.results, [
            'I want to play Minecraft.\nMake my upper swipe move forward.\n'
            'Make my lower swipe move backward.\nAnd make my left swipe move left'])

    def test_stand_alone_finish(self):
        self.collector.on_line('hey gemma')
        self.collector.on_line('map swipe up to W')
        self.collector.on_line('Gemma')
        self.assertEqual(self.results, ['map swipe up to W'])

    def test_silence(self):
        self.collector.on_line('Hi, Gemma')
        self.collector.on_line('Map swipe up to W')
        self.advance(5.9)
        self.assertEqual(self.results, [])
        self.advance(0.2)
        self.assertEqual(self.results, ['Map swipe up to W'])

    def test_partial_prevents_premature_timeout(self):
        self.collector.on_line('Hi Gemma map swipe up to W')
        self.advance(5)
        self.collector.on_partial('and')
        self.advance(4)
        self.assertEqual(self.results, [])
        self.collector.on_line('and swipe down to S')
        self.advance(6.1)
        self.assertEqual(len(self.results), 1)

    def test_reject_without_wake(self):
        self.collector.on_line('Someone said Hi Gemma on the TV')
        self.collector.on_line('Gemma do that')
        self.collector.on_line('The word gemma was here')
        self.advance(10)
        self.assertEqual(self.results, [])

    def test_expired_wake(self):
        self.collector.on_line('Hi, Gemma')
        self.advance(15.1)
        self.collector.on_line('map up to w')
        self.assertEqual(self.results, [])

    def test_cancel(self):
        self.collector.on_line('Hi Gemma map up to W')
        self.collector.on_line('cancel gemma')
        self.assertEqual(self.results, [])
        self.assertFalse(self.collector.collecting)

    def test_finish_with_no_command(self):
        self.collector.on_line('Hi Gemma')
        self.collector.on_line('Gemma do that')
        self.assertEqual(self.results, [])

    def test_no_duplicate_lines(self):
        self.collector.on_line('Hi Gemma')
        self.collector.on_line('Map up to W')
        self.collector.on_line('Map up to W')
        self.collector.on_line('Gemma')
        self.assertEqual(self.results, ['Map up to W'])


if __name__ == '__main__':
    unittest.main()
