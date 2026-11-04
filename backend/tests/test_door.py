import tempfile
import unittest
from backend.store import Store
from backend.door import observe,summarize

class DoorTests(unittest.TestCase):
    def test_baseline_changes_retry_and_stale(self):
        with tempfile.TemporaryDirectory() as tmp:
            store=Store(tmp+'/db')
            observe(store,'live','room',{'door':'CLOSED','body':0},100)
            self.assertEqual(store.list_docs('live','room','door_record'),[])
            observe(store,'live','room',{'door':'OPEN'},200)
            observe(store,'live','room',{'door':'OPEN'},210)
            self.assertEqual(len(store.list_docs('live','room','door_record')),1)
            state=store.doc('live','room','door_context')
            self.assertEqual(summarize(state,400)['level'],'unknown')
            observe(store,'live','room',{'door':'OPEN','body':1},810)
            state=store.doc('live','room','door_context')
            self.assertIn('10分钟',summarize(state,811)['hint'])
            self.assertEqual(state['last_activity_at'],810)
            observe(store,'live','room',{'door':'CLOSED'},820)
            self.assertEqual(len(store.list_docs('live','room','door_record')),2)
            self.assertEqual(store.doc('live','room','shadow'),None)

class LightControlTests(unittest.TestCase):
    def test_manual_control_priority_and_validation(self):
        from backend.medicine import LightControl,mock_living_command
        from pydantic import ValidationError
        for invalid in [True,2,-2,'1']:
            with self.assertRaises(ValidationError): LightControl.model_validate({'mode':invalid})
        life,result=mock_living_command(None,'set_light',{'mode':1},1800000000,500,0)
        self.assertEqual(result,0);self.assertTrue(life['night_on'])
        life,result=mock_living_command(life,'set_light',{'mode':1},1800000000,500,1)
        self.assertEqual(life['light_owner'],'alarm')
        life,result=mock_living_command(life,'set_light',{'mode':0},1800000000,0,0)
        self.assertFalse(life['night_on'])
    def test_failed_settings_do_not_reset_manual_light(self):
        from backend.medicine import mock_living_command
        life,_=mock_living_command(None,'set_light',{'mode':1},1800000000,500,0)
        life,result=mock_living_command(life,'set_living',{'expected_revision':999,'night_auto':False,'medicine_enabled':False,'medicine_minute':480},1800000000,500,0)
        self.assertEqual(result,4)
        self.assertEqual(life['light_mode'],1)
        self.assertTrue(life['night_on'])
