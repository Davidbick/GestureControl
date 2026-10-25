"""Bounded adaptation of Google's official Tokyo weather sequence.

Run explicitly; never runs inference on import. The weather function is a mock.
"""
import json
from transformers_backend import ACTIVATION, FunctionGemma, parse_call


def get_current_weather(location: str, unit: str = "celsius"):
    """Gets the current weather in a given location.

    Args:
        location: The city and state, e.g. "San Francisco, CA" or "Tokyo, JP"
        unit: The unit to return the temperature in. (choices: ["celsius", "fahrenheit"])

    Returns:
        temperature: The current temperature in the given location
        weather: The current weather in the given location
    """
    return {"temperature": 15, "weather": "sunny"}


def validate_weather(call):
    if call['name'] != 'get_current_weather':
        raise ValueError('Unexpected weather tool')
    args = call['arguments']
    if set(args) - {'location','unit'} or 'location' not in args:
        raise ValueError('Unexpected weather arguments')
    if args['location'] not in ('Tokyo', 'Tokyo, Japan', 'Tokyo, JP'):
        raise ValueError('Expected Tokyo')
    if args.get('unit','celsius') != 'celsius':
        raise ValueError('Mock result requires Celsius')
    return args


def main():
    model = FunctionGemma()
    tools = [get_current_weather]
    messages = [{'role':'developer','content':ACTIVATION},
                {'role':'user','content':"Hey, what's the weather in Tokyo right now?"}]
    generated = model.generate(messages, tools)
    print('RAW CALL:', generated.text, flush=True)
    call = parse_call(generated.text)
    args = validate_weather(call)
    result = get_current_weather(**args)
    print('VALIDATED CALL:', json.dumps(call), flush=True)
    print('MOCK RESULT:', json.dumps(result), flush=True)
    messages.append({'role':'assistant','tool_calls':[{'type':'function','function':call}]})
    messages.append({'role':'tool','content':{'name':call['name'],'response':result}})
    final = model.generate(messages, tools)
    print('FINAL:', final.text, flush=True)
    if '<start_function_call>' in final.text or not all(
        word in final.text.lower() for word in ('tokyo','15','sunny')
    ):
        raise ValueError('Final response did not summarize the mock result')
    print('WEATHER: PASS', flush=True)


if __name__ == '__main__':
    main()
