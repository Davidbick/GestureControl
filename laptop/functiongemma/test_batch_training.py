import json
import unittest
from train_batch import DATA, tools, signature


class DatasetTests(unittest.TestCase):
    def test_schema_is_existing_batch_tool(self):
        from train_batch import ROOT
        protocol=json.loads((ROOT/'protocol/functiongemma_tools.json').read_text())
        self.assertEqual(tools(),[t for t in protocol if t['function']['name']=='configure_gesture_batch'])

    def test_data_valid_and_heldout_separate(self):
        splits={name:json.loads((DATA/f'{name}.json').read_text()) for name in ('train','heldout')}
        calls={}
        for name, rows in splits.items():
            self.assertTrue(rows)
            self.assertEqual(len({r['prompt'] for r in rows}),len(rows))
            calls[name]={json.dumps(signature(r['call'])) for r in rows}
        self.assertFalse(calls['train'] & calls['heldout'])
        self.assertFalse({r['prompt'] for r in splits['train']} & {r['prompt'] for r in splits['heldout']})

    def test_training_covers_all_batch_sizes(self):
        rows=json.loads((DATA/'train.json').read_text())
        sizes={len(r['call']['arguments']['mappings']) for r in rows}
        self.assertTrue({1,2,3,7} <= sizes)


if __name__ == '__main__':
    unittest.main()
