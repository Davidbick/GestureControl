import unittest
import contextlib
import sys
from types import SimpleNamespace
from unittest.mock import Mock, patch
from transformers_backend import FunctionGemma, parse_call
from transformers_weather import validate_weather


class ParserTests(unittest.TestCase):
    def wrap(self, body):
        return '<start_function_call>call:' + body + '<end_function_call>'

    def test_official_weather(self):
        call=parse_call(self.wrap('get_current_weather{location:<escape>Tokyo, Japan<escape>}'))
        self.assertEqual(validate_weather(call), {'location':'Tokyo, Japan'})

    def test_nested_batch(self):
        call=parse_call(self.wrap('configure_gesture_batch{target:<escape>COMPUTER<escape>,mappings:[{gesture:<escape>SWIPE_UP<escape>,action:<escape>KEY_TAP<escape>,key:<escape>W<escape>}]}'))
        self.assertEqual(call['arguments']['mappings'][0]['key'],'W')

    def test_rejects_invalid_or_repeated(self):
        good=self.wrap('tool{x:<escape>value<escape>}')
        for text in (good+good,good+'junk','prefix'+good,good[:-5],
                     self.wrap('tool{x:bare}'),self.wrap('tool{x:1,x:2}'),
                     self.wrap('tool{x:1,}'),self.wrap('tool{x:[1,]}')):
            with self.subTest(text=text),self.assertRaises(ValueError):
                parse_call(text)

    def test_string_punctuation_preserved(self):
        call=parse_call(self.wrap('tool{x:<escape>a,{b}:"c"<escape>}'))
        self.assertEqual(call['arguments']['x'],'a,{b}:"c"')

    def test_rejects_wrong_weather(self):
        for call in ({'name':'other','arguments':{}},
                     {'name':'get_current_weather','arguments':{'location':'London'}},
                     {'name':'get_current_weather','arguments':{'location':'Tokyo','extra':1}}):
            with self.assertRaises(ValueError):
                validate_weather(call)


class GenerationTests(unittest.TestCase):
    def backend(self, generated):
        class Tokens(list):
            def __getitem__(self, key):
                result=super().__getitem__(key)
                return Tokens(result) if isinstance(key,slice) else result
            def tolist(self):
                return list(self)
        class Inputs(dict):
            def to(self, device):
                return self
        backend=FunctionGemma.__new__(FunctionGemma)
        backend.processor=Mock(eos_token_id=1)
        backend.processor.apply_chat_template.return_value=Inputs(input_ids=SimpleNamespace(shape=(1,2)))
        backend.model=Mock(device='cpu')
        backend.model.generation_config.eos_token_id=[1,50,106]
        backend.model.generate.return_value=[Tokens([2,99]+generated)]
        return backend

    def test_preserves_call_tokens_and_uses_bounds(self):
        backend=self.backend([48,52,49,50])
        with patch.dict(sys.modules,torch=SimpleNamespace(inference_mode=contextlib.nullcontext)):
            result=backend.generate([],[])
        backend.processor.decode.assert_called_once_with([48,52,49],skip_special_tokens=False)
        args=backend.model.generate.call_args.kwargs
        self.assertEqual(args['max_new_tokens'],128)
        self.assertEqual(args['max_time'],30)
        self.assertFalse(args['do_sample'])
        self.assertEqual(result.tokens,4)

    def test_rejects_time_or_token_exhaustion(self):
        for generated in ([48,52,49],[48]*127+[50],[]):
            backend=self.backend(generated)
            with patch.dict(sys.modules,torch=SimpleNamespace(inference_mode=contextlib.nullcontext)):
                with self.assertRaises(ValueError):
                    backend.generate([],[])


if __name__ == '__main__':
    unittest.main()
