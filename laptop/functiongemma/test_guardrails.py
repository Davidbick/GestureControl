"""Offline contract tests. Network and BLE are never required."""
import copy
import unittest
from unittest.mock import Mock, patch
from ollama import ChatResponse
import test_inference as inference


def response(call=None, **overrides):
    call = copy.deepcopy(call or inference.CASES[0][1])
    data = {"done": True, "done_reason": "stop", "eval_count": 50,
            "message": {"role": "assistant", "content": "",
                        "tool_calls": [{"function": call}]}}
    data.update(overrides)
    return ChatResponse(**data)


class GuardrailsTests(unittest.TestCase):
    def test_all_expected_calls_use_existing_validators(self):
        for _, call in inference.CASES:
            with self.subTest(name=call['name']):
                result=inference.validated_calls(response(call))
                self.assertEqual(result, [call])

    def test_single_wire_payload(self):
        call=inference.validated_calls(response())[0]
        self.assertEqual(inference.signature(call),
                         ('set_gesture_mapping','COMPUTER','020101011a0000230000'))

    def test_batch_matches_individual_encodings(self):
        call=inference.CASES[1][1]
        validated=inference.validated_calls(response(call))[0]
        sig=inference.signature(validated)
        expected=[]
        for mapping in call['arguments']['mappings']:
            single={'name':'set_gesture_mapping','arguments':{'target':'COMPUTER',**mapping}}
            expected.append((mapping['gesture'],inference.signature(single)[2]))
        self.assertEqual(sig,('batch','COMPUTER',tuple(sorted(expected))))

    def test_rejects_empty_and_repeated_calls(self):
        for count in (0,2,10):
            r=response()
            r.message.tool_calls=(r.message.tool_calls or [])*count
            with self.subTest(count=count), self.assertRaises(ValueError):
                inference.validated_calls(r)

    def test_rejects_incomplete_even_with_valid_call(self):
        for change in ({'done':False},{'done_reason':'length'},
                       {'done_reason':None},{'eval_count':inference.MAX_TOKENS}):
            with self.subTest(change=change),self.assertRaises(ValueError):
                inference.validated_calls(response(**change))

    def test_rejects_text_and_malformed_tail(self):
        for text in ('Here you go','<start_function_call>call:broken{'):
            r=response()
            r.message.content=text
            with self.assertRaises(ValueError):
                inference.validated_calls(r)

    def test_rejects_unknown_name(self):
        for name in ('','execute_ble','set_gesture_mapping '):
            call=copy.deepcopy(inference.CASES[0][1])
            call['name']=name
            with self.assertRaises(ValueError):
                inference.validated_calls(response(call))

    def test_rejects_bad_arguments(self):
        for change in ({'target':'computer'},{'gesture':'FAKE'},
                       {'action':'LIGHT_ON'},{'key':'UNKNOWN'},
                       {'duration_ms':True},{'unexpected':1}):
            call=copy.deepcopy(inference.CASES[0][1])
            call['arguments'].update(change)
            with self.subTest(change=change),self.assertRaises(ValueError):
                inference.validated_calls(response(call))

    def test_batch_rejects_partial_invalid_and_duplicate(self):
        for duplicate in (False,True):
            call=copy.deepcopy(inference.CASES[1][1])
            mappings=call['arguments']['mappings']
            if duplicate:
                mappings[1]=mappings[0].copy()
            else:
                mappings[-1]['action']='INVALID'
            with self.assertRaises(ValueError):
                inference.validated_calls(response(call))

    def test_request_contract(self):
        client=Mock()
        client.chat.return_value=response(inference.CASES[1][1])
        with patch.object(inference,'Client',return_value=client) as factory:
            self.assertTrue(inference.run(*inference.CASES[1]))
        factory.assert_called_once_with(timeout=30)
        args=client.chat.call_args.kwargs
        self.assertEqual(args['messages'][0]['role'],'developer')
        self.assertTrue(args['messages'][0]['content'].startswith(inference.ACTIVATION))
        self.assertEqual(len(args['tools']),4)
        self.assertEqual(args['options']['num_predict'],256)
        self.assertFalse(args['stream'])

    def test_valid_but_wrong_intent_fails(self):
        client=Mock()
        client.chat.return_value=response(inference.CASES[2][1])
        with patch.object(inference,'Client',return_value=client):
            self.assertFalse(inference.run(*inference.CASES[0]))


if __name__ == '__main__':
    unittest.main()
