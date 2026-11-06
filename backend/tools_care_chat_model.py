"""Review/add remote care commands and extend contact for text; default dry run.
Run with the server's private configuration: python tools_care_chat_model.py [--apply]
"""
import argparse
import copy
import json
from datetime import datetime,timezone
from pathlib import Path
from backend.settings import Settings
from backend.cloud import HuaweiCloud
from huaweicloudsdkiotda.v5 import ShowDeviceRequest,ShowProductRequest,UpdateProductRequest,UpdateProduct,ServiceCommand,ServiceCommandPara

def additions():
    def p(name,kind,low=None,high=None,required=True,enum=None,length=None):
        return ServiceCommandPara(para_name=name,data_type=kind,required=required,min=str(low) if low is not None else None,max=str(high) if high is not None else None,enum_list=enum,max_length=length)
    return [ServiceCommand(command_name='contact',paras=[p('action','string',enum=['reply','seen','chat']),p('boot_id','int',1,2147483647),p('message_id','int',0,2147483647),p('code','int',0,4,False),p('request_id','string',required=False,length=64),p('text','string',required=False,length=120),p('author','string',required=False,length=32)]),
            ServiceCommand(command_name='set_care_policy',paras=[p('boot_id','int',1,2147483647),p('enabled','boolean'),p('sleep_enabled','boolean'),p('start','int',0,1439),p('end','int',0,1439),p('quiet','int',0,1),p('noact_ms','int',10000,86400000),p('expected_revision','int',1,999999998)]),
            ServiceCommand(command_name='pause_care',paras=[p('boot_id','int',1,2147483647),p('minutes','int',0,1440),p('expected_pause_revision','int',1,999999998)])]

def extend_services(services,service_id):
    proposed=copy.deepcopy(services)
    target=next((s for s in proposed if s.service_id==service_id),None)
    if target is None:raise ValueError('Service not found')
    commands=list(target.commands or []);changed=[]
    for wanted in additions():
        old=next((c for c in commands if c.command_name==wanted.command_name),None)
        if old is None:commands.append(wanted);changed.append(wanted.command_name);continue
        wanted_names={p.para_name for p in wanted.paras}
        if any(p.required and p.para_name not in wanted_names for p in old.paras or []):raise ValueError('Unexpected required parameter: '+wanted.command_name)
        before=old.to_dict();known={p.para_name:p for p in old.paras or []}
        for param in wanted.paras:
            previous=known.get(param.para_name)
            if previous and previous.data_type!=param.data_type:raise ValueError('Parameter type differs: '+wanted.command_name+'/'+param.para_name)
            known[param.para_name]=param
        old.paras=list(known.values())
        if old.to_dict()!=before:changed.append(wanted.command_name)
    target.commands=commands
    return proposed,changed

def retained(before,after,service_id):
    left={s.service_id:s.to_dict() for s in before};right={s.service_id:s.to_dict() for s in after}
    if left.keys()!=right.keys():return False
    managed={c.command_name for c in additions()}
    for key,value in left.items():
        current=right[key]
        for name in value:
            if key==service_id and name=='commands':continue
            if value[name]!=current.get(name):return False
        if key==service_id:
            old={c['command_name']:c for c in value.get('commands') or []};new={c['command_name']:c for c in current.get('commands') or []}
            if not set(old).issubset(new):return False
            for name,command in old.items():
                if name not in managed and command!=new[name]:return False
    return True

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--apply',action='store_true');args=parser.parse_args()
    settings=Settings.load();settings.validate()
    if settings.mode!='live':raise SystemExit('Use the existing live server configuration')
    try:
        client=HuaweiCloud(settings).client
        pid=client.show_device(ShowDeviceRequest(device_id=settings.primary_device)).product_id
        product=client.show_product(ShowProductRequest(product_id=pid))
        proposed,changed=extend_services(product.service_capabilities,settings.service_id)
        if not retained(product.service_capabilities,proposed,settings.service_id):raise ValueError('Unrelated model fields changed')
        print('Commands to add/extend:',', '.join(changed) or 'none')
        if not changed:return
        if not args.apply:print('Dry run only. Review and use --apply to save.');return
        folder=Path(__file__).resolve().parent/'.local/model-backups';folder.mkdir(parents=True,exist_ok=True)
        backup=folder/('care-chat-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')+'.json');backup.write_text(json.dumps(product.to_dict(),ensure_ascii=False,indent=2))
        client.update_product(UpdateProductRequest(product_id=pid,body=UpdateProduct(service_capabilities=proposed)))
        actual=client.show_product(ShowProductRequest(product_id=pid))
        _,missing=extend_services(actual.service_capabilities,settings.service_id)
        if missing or not retained(product.service_capabilities,actual.service_capabilities,settings.service_id):raise ValueError('Readback verification failed; inspect backup')
        print('Verified care/chat commands; existing properties and other commands retained. Backup:',backup.name)
    except Exception as exc:raise SystemExit('Cloud model operation stopped ('+type(exc).__name__+'). Check configuration/network locally; no credentials printed.') from None
if __name__=='__main__':main()
