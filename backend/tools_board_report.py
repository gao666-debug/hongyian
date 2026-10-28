"""Change only a LAN board's report interval, optionally restoring it later."""
import argparse
import json
import time
from urllib.request import Request, urlopen


def status(base):
    with urlopen(base + '/status', timeout=5) as response:
        return json.load(response)


def set_interval(base, interval):
    request = Request(base + '/config',
                      data=json.dumps({'report_ms': interval}).encode(),
                      headers={'Content-Type': 'application/json'}, method='POST')
    with urlopen(request, timeout=8) as response:
        result = json.load(response)
    if result.get('ok') is not True or status(base).get('report_ms') != interval:
        raise RuntimeError('Board interval update failed verification')
    print(json.dumps({'report_ms': interval}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', default='http://10.125.126.36:8080')
    parser.add_argument('--seconds', type=int, required=True)
    parser.add_argument('--restore-after-minutes', type=int, default=0)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or args.restore_after_minutes < 0:
        parser.error('Invalid interval or duration')
    previous = status(args.base).get('report_ms')
    if type(previous) is not int:
        raise RuntimeError('Missing previous report interval')
    target = args.seconds * 1000
    set_interval(args.base, target)
    if args.restore_after_minutes:
        deadline = time.time() + args.restore_after_minutes * 60
        print(json.dumps({'restore_at_epoch': deadline, 'restore_report_ms': previous}), flush=True)
        while time.time() < deadline:
            time.sleep(min(30, max(0, deadline - time.time())))
        # Do not overwrite settings changed separately by the user.
        for attempt in range(10):
            try:
                if status(args.base).get('report_ms') == target:
                    set_interval(args.base, previous)
                    print('Previous report interval restored', flush=True)
                else:
                    print('Restore skipped: settings changed separately', flush=True)
                return
            except Exception:
                print('Restore connection failed; retrying', flush=True)
                time.sleep(30)
        raise RuntimeError('Automatic restore failed; run manual restore when board is reachable')


if __name__ == '__main__':
    main()
