"""Add only the remote SOS command to the existing gateway product, preserving its model.
Usage: .venv/bin/python tools_sos_model.py [--apply]
Credentials are read from private .env. No device MQTT connection is opened.
"""
import argparse
import json
from pathlib import Path
from datetime import datetime, timezone
from backend.settings import Settings
from backend.cloud import HuaweiCloud
from huaweicloudsdkiotda.v5 import (ShowDeviceRequest, ShowProductRequest,
    UpdateProductRequest, UpdateProduct, ServiceCommand, ServiceCommandPara)


def additions():
    return [ServiceCommand(command_name='sos',paras=[])]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply',action='store_true')
    args=parser.parse_args()
    settings=Settings.load();settings.validate()
    if settings.mode!='live':raise SystemExit('BACKEND_MODE must be live')
    client=HuaweiCloud(settings).client
    try:
        product_id=client.show_device(ShowDeviceRequest(device_id=settings.primary_device)).product_id
        product=client.show_product(ShowProductRequest(product_id=product_id))
        service=next((s for s in product.service_capabilities if s.service_id=='HomeSafety'),None)
        if service is None:raise SystemExit('HomeSafety service not found; nothing changed')
        wanted=additions();existing={c.command_name:c for c in service.commands or []}
        missing=[]
        for command in wanted:
            if command.command_name not in existing:missing.append(command)
            else:
                expected={(p.para_name,p.data_type,p.required) for p in command.paras}
                actual={(p.para_name,p.data_type,p.required) for p in existing[command.command_name].paras or []}
                if expected!=actual:raise SystemExit('Existing command differs: '+command.command_name+'; nothing changed')
        print('Missing commands:',', '.join(c.command_name for c in missing) or 'none')
        if not missing:return
        if not args.apply:print('Dry run. Use --apply to add these commands.');return
        backup=Path(__file__).resolve().parent/'.local/model-backups'
        backup.mkdir(parents=True,exist_ok=True)
        name=backup/('product-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'.json')
        name.write_text(json.dumps(product.to_dict(),ensure_ascii=False,indent=2))
        service.commands=list(service.commands or [])+missing
        # Existing fields, properties, events and commands are carried through unchanged.
        body=UpdateProduct(service_capabilities=product.service_capabilities)
        client.update_product(UpdateProductRequest(product_id=product_id,body=body))
        result=client.show_product(ShowProductRequest(product_id=product_id))
        names={c.command_name for s in result.service_capabilities if s.service_id=='HomeSafety' for c in s.commands or []}
        if not all(c.command_name in names for c in wanted):raise SystemExit('Verification failed; inspect saved model backup')
        print('Verified SOS command. Existing properties/events retained. Backup:',name.name)
    except SystemExit:raise
    except Exception as exc:
        # SDK exceptions may contain signed request metadata. Never dump them.
        raise SystemExit('Cloud model operation failed ('+type(exc).__name__+'). Check API permissions/network locally.') from None

if __name__=='__main__':main()
