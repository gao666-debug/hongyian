import json
import unittest
from decimal import Decimal
from backend.cloud import json_numbers


class CloudNumbersTests(unittest.TestCase):
    def test_sdk_shadow_properties_can_be_stored_as_json(self):
        raw = {'temp': Decimal('32.125'), 'body': 1,
               'nested': [{'humi': Decimal('45.5')}]}
        actual = json.loads(json.dumps(json_numbers(raw)))
        self.assertEqual(actual, {'temp': 32.125, 'body': 1,
                                  'nested': [{'humi': 45.5}]})
        self.assertIsInstance(raw['temp'], Decimal)
