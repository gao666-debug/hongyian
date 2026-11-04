"""Door observations, not a determination of a resident's location."""
from datetime import datetime, timezone, timedelta

def observe(store, mode, device, props, now):
    with store.connection() as db:
        db.execute('BEGIN IMMEDIATE')
        row=db.execute('SELECT body FROM documents WHERE mode=? AND device=? AND kind=? AND key=?',
                       (mode,device,'door_context','latest')).fetchone()
        import json
        state=json.loads(row['body']) if row else {}
        if props.get('body')==1: state['last_activity_at']=now
        if props.get('door') in ('OPEN','CLOSED'):
            current=props['door']
            previous=state.get('door')
            if previous!=current:
                # First reading establishes a baseline, never fabricates an open/close.
                if previous is not None:
                    event={'state':current,'previous':previous,'observed_at':now}
                    store.put(mode,device,'door_record',event,str(now),db)
                state.update(door=current,changed_at=now)
            state['received_at']=now
        store.put(mode,device,'door_context',state,db=db)
        db.execute("DELETE FROM documents WHERE mode=? AND device=? AND kind='door_record' AND key NOT IN (SELECT key FROM documents WHERE mode=? AND device=? AND kind='door_record' ORDER BY updated DESC LIMIT 1000)",
                   (mode,device,mode,device))

def summarize(state, now):
    result={'door':state.get('door'),'received_at':state.get('received_at'),
            'last_activity_at':state.get('last_activity_at'),'hint':'等待门窗上报','level':'unknown'}
    if not state.get('received_at') or now-state['received_at']>90:
        result['hint']='门窗数据过期或尚未上报，无法判断';return result
    result.update(level='normal',hint='门窗已关；开关记录不能单独判断是否外出')
    if state['door']=='OPEN':
        elapsed=max(0,int(now-state['changed_at']))
        hour=datetime.fromtimestamp(now,timezone(timedelta(hours=8))).hour
        result.update(hint='门窗开启，请结合活动情况核实',level='attention')
        if elapsed>=600: result['hint']='门窗已持续开启10分钟以上，请核实是否需要关好'
        elif hour>=22 or hour<6: result['hint']='夜间门窗开启，请核实是否为正常出入'
    return result
