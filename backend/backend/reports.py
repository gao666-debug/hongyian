"""Observed activity summaries. Missing dates are never treated as zero activity."""
from datetime import date, timedelta


def summarize(days, end, length):
    start = end - timedelta(days=length - 1)
    selected = [d for d in days if start.isoformat() <= d['date'] <= end.isoformat()]
    hours = [sum(d['activity_by_hour'][h] for d in selected) for h in range(24)]
    total = sum(d['activity_total'] for d in selected)
    return {'from_date': start.isoformat(), 'to_date': end.isoformat(),
            'observed_days': len(selected), 'calendar_days': length,
            'activity_total': total, 'alarm_total': sum(d['alarm_total'] for d in selected),
            'daily_average': round(total / len(selected), 1) if selected else None,
            'activity_by_hour': hours,
            'peak_hour': hours.index(max(hours)) if any(hours) else None,
            'coverage': 'observed_only'}


def build_report(days, today):
    # Dated buckets may cover only part of a day. Never diagnose health or compare
    # today's partial bucket to a completed day as if their coverage were equal.
    by_date = {d['date']: d for d in days}
    yesterday = today - timedelta(days=1)
    previous = yesterday - timedelta(days=1)
    current, baseline = by_date.get(yesterday.isoformat()), by_date.get(previous.isoformat())
    change = None
    if current is not None and baseline is not None and baseline['activity_total'] > 0:
        change = round((current['activity_total'] - baseline['activity_total']) * 100 / baseline['activity_total'], 1)
    return {'coverage': 'observed_only', 'today': today.isoformat(),
            'comparison': {'date': yesterday.isoformat(), 'baseline_date': previous.isoformat(),
                           'activity_total': current['activity_total'] if current else None,
                           'baseline_total': baseline['activity_total'] if baseline else None,
                           'change_percent': change,
                           'comparable': False,
                           'note': '按已观察次数比较；缺少完整在线时长，不能据此判定活动量或健康变化'},
            'week': summarize(days, yesterday, 7), 'month': summarize(days, yesterday, 30),
            'advice': '次数持续下降时，请先核对设备在线和覆盖情况，再联系老人确认；本报告不用于医疗诊断。'}
