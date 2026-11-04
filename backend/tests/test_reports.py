import unittest
from datetime import date
from backend.reports import build_report


def day(stamp, count):
    return {'date':stamp,'activity_total':count,'alarm_total':1,'activity_by_hour':[0]*8+[count]+[0]*15}


class ReportsTests(unittest.TestCase):
    def test_calendar_windows_and_missing_days(self):
        result=build_report([day('2026-11-03',80),day('2026-11-02',100),day('2026-10-05',20),day('2026-11-04',999)],date(2026,11,4))
        self.assertEqual(result['comparison']['change_percent'],-20)
        self.assertFalse(result['comparison']['comparable'])
        self.assertEqual(result['week']['activity_total'],180)
        self.assertEqual(result['week']['observed_days'],2)
        self.assertEqual(result['month']['activity_total'],200)
        self.assertEqual(result['month']['calendar_days'],30)
        self.assertEqual(result['month']['peak_hour'],8)

    def test_unknown_is_not_zero_and_zero_baseline(self):
        for days in ([],[day('2026-11-03',20)],[day('2026-11-03',20),day('2026-11-02',0)]):
            result=build_report(days,date(2026,11,4))
            self.assertIsNone(result['comparison']['change_percent'])
        self.assertIsNone(build_report([],date(2026,11,4))['week']['daily_average'])

    def test_year_and_leap_month_boundaries(self):
        result=build_report([day('2024-02-29',12),day('2024-03-01',6)],date(2024,3,2))
        self.assertEqual(result['comparison']['change_percent'],-50)
        self.assertEqual(result['week']['observed_days'],2)
        self.assertEqual(build_report([],date(2026,1,1))['week']['to_date'],'2025-12-31')

class ReportApiTests(unittest.TestCase):
    def test_reports_require_login_and_read_persisted_days(self):
        import tempfile
        from fastapi.testclient import TestClient
        from backend.app import create_app
        from backend.settings import Settings
        with tempfile.TemporaryDirectory() as tmp:
            settings=Settings(db=tmp+'/test.sqlite3',password='test-only-password',webhook_token='x'*32)
            with TestClient(create_app(settings)) as client:
                path='/api/v1/devices/'+settings.primary_device+'/reports'
                self.assertEqual(client.get(path).status_code,401)
                token=client.post('/api/v1/auth/login',json={'username':'guardian','password':settings.password}).json()['data']['access_token']
                auth={'Authorization':'Bearer '+token}
                response=client.get(path,headers=auth)
                self.assertEqual(response.status_code,200)
                self.assertEqual(response.json()['data']['week']['observed_days'],0)
                self.assertIsNone(response.json()['data']['comparison']['change_percent'])
                self.assertEqual(client.get('/api/v1/devices/unknown/reports',headers=auth).status_code,404)
