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
        self.collector.on_line('Hey Gesture, I want to play Minecraft.')
        self.collector.on_line('Make my upper swipe move forward.')
        self.advance(2)
        self.collector.on_line('Make my lower swipe move backward.')
        self.advance(3)
        self.collector.on_line('And make my left swipe move left. Gesture do that.')
        self.assertEqual(self.results, [
            'I want to play Minecraft.\nMake my upper swipe move forward.\n'
            'Make my lower swipe move backward.\nAnd make my left swipe move left'])

    def test_stand_alone_finish(self):
        self.collector.on_line('hey gesture')
        self.collector.on_line('map swipe up to W')
        self.collector.on_line('Gesture')
        self.assertEqual(self.results, ['map swipe up to W'])

    def test_silence(self):
        self.collector.on_line('Hey Gesture')
        self.collector.on_line('Map swipe up to W')
        self.advance(5.9)
        self.assertEqual(self.results, [])
        self.advance(0.2)
        self.assertEqual(self.results, ['Map swipe up to W'])

    def test_partial_prevents_premature_timeout(self):
        self.collector.on_line('Hey Gesture map swipe up to W')
        self.advance(5)
        self.collector.on_partial('and')
        self.advance(4)
        self.assertEqual(self.results, [])
        self.collector.on_line('and swipe down to S')
        self.advance(6.1)
        self.assertEqual(len(self.results), 1)

    def test_reject_without_wake(self):
        self.collector.on_line('Someone said Hey Gesture on the TV')
        self.collector.on_line('Gesture do that')
        self.collector.on_line('The word gesture was here')
        self.advance(10)
        self.assertEqual(self.results, [])

    def test_expired_wake(self):
        self.collector.on_line('Hey Gesture')
        self.advance(15.1)
        self.collector.on_line('map up to w')
        self.assertEqual(self.results, [])

    def test_cancel(self):
        self.collector.on_line('Hey Gesture map up to W')
        self.collector.on_line('cancel gesture')
        self.assertEqual(self.results, [])
        self.assertFalse(self.collector.collecting)

    def test_finish_with_no_command(self):
        self.collector.on_line('Hey Gesture')
        self.collector.on_line('Gesture do that')
        self.assertEqual(self.results, [])

    def test_no_duplicate_lines(self):
        self.collector.on_line('Hey Gesture')
        self.collector.on_line('Map up to W')
        self.collector.on_line('Map up to W')
        self.collector.on_line('Gesture')
        self.assertEqual(self.results, ['Map up to W'])


    def test_old_wake_word_is_rejected(self):
        self.collector.on_line('Hi Gemma, map up to W')
        self.collector.on_line('Gesture')
        self.assertEqual(self.results, [])
        self.assertFalse(self.collector.collecting)

    def test_new_wake_and_explicit_finish(self):
        self.collector.on_line('Hey Gesture, map up to W. Gesture do that')
        self.assertEqual(self.results, ['map up to W'])


    def test_hey_control_wakes_and_control_finishes(self):
        self.collector.on_line('Hey Control, make swipe up press W.')
        self.collector.on_line('Control')
        self.assertEqual(self.results, ['make swipe up press W.'])

    def test_gesture_wake_control_finish(self):
        self.collector.on_line('Hey Gesture')
        self.collector.on_line('Make swipe down press S')
        self.collector.on_line('Control, do that')
        self.assertEqual(self.results, ['Make swipe down press S'])

    def test_control_wake_gesture_finish(self):
        self.collector.on_line('Hey Control make swipe left press A')
        self.collector.on_line('Gesture, do that')
        self.assertEqual(self.results, ['make swipe left press A'])

    def test_hi_and_high_are_not_wake_words(self):
        for wake in ('Hi Gesture', 'Hi Control', 'High Gesture',
                     'High Control', 'Hey Gemma'):
            with self.subTest(wake=wake):
                collector_results = []
                collector = CommandCollector(collector_results.append)
                collector.on_line(wake + ', map up to W')
                collector.on_line('Control')
                self.assertEqual(collector_results, [])
                self.assertFalse(collector.collecting)

    def test_control_cancel(self):
        self.collector.on_line('Hey Control, make up press W')
        self.collector.on_line('Cancel, Control')
        self.assertFalse(self.collector.collecting)
        self.assertEqual(self.results, [])

    def test_unrelated_control_word_does_not_finish(self):
        self.collector.on_line('Hey Gesture')
        self.collector.on_line('Set up control for Minecraft')
        self.assertEqual(self.results, [])
        self.collector.on_line('Control do that')
        self.assertEqual(self.results, ['Set up control for Minecraft'])

    def test_hey_control_no_command(self):
        self.collector.on_line('Hey Control')
        self.collector.on_line('Control')
        self.assertEqual(self.results, [])

    def test_control_is_not_a_wake_word_alone(self):
        self.collector.on_line('Control')
        self.collector.on_line('map up to W')
        self.assertFalse(self.collector.collecting)
        self.assertEqual(self.results, [])


if __name__ == '__main__':
    unittest.main()
