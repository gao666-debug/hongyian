"""Three independent daily reminders; legacy fields always describe slot zero."""
from typing import Literal
from pydantic import BaseModel, ConfigDict, Field, StrictBool, StrictInt, model_validator

class StrictModel(BaseModel):
    model_config = ConfigDict(extra='forbid')

class LivingConfig(StrictModel):
    night_auto: StrictBool
    medicine_enabled: StrictBool
    medicine_minute: StrictInt = Field(ge=0,le=1439)
    expected_revision: StrictInt = Field(ge=1,lt=1000000000)

class MedicinePlan(LivingConfig):
    medicine_enabled_2: StrictBool
    medicine_minute_2: StrictInt = Field(ge=0,le=1439)
    medicine_enabled_3: StrictBool
    medicine_minute_3: StrictInt = Field(ge=0,le=1439)

class MedicineConfirmation(StrictModel):
    dose_day: StrictInt = Field(ge=0,le=1000000)

class MedicineChange(MedicineConfirmation):
    slot: StrictInt = Field(ge=0,le=2)
    taken: StrictBool
    expected_medicine_revision: StrictInt = Field(ge=1,lt=1000000000)

class MedicineSlot(StrictModel):
    slot: StrictInt = Field(ge=0,le=2)
    enabled: StrictBool
    minute: StrictInt = Field(ge=0,le=1439)
    state: StrictInt = Field(ge=0,le=4)
    confirmed_epoch: StrictInt = Field(ge=0,le=4294967295)

class LivingStatus(StrictModel):
    schema_version: int = Field(default=1,alias='schema',ge=1,le=2)
    config_revision: StrictInt = Field(ge=1,le=1000000000)
    night_auto: StrictBool
    night_on: StrictBool
    light_valid: StrictBool
    light_owner: Literal['off','night','medicine','alarm']
    medicine_enabled: StrictBool
    medicine_minute: StrictInt = Field(ge=0,le=1439)
    medicine_state: StrictInt = Field(ge=0,le=4)
    dose_day: StrictInt = Field(ge=-1,le=1000000)
    confirmed_epoch: StrictInt = Field(ge=0)
    time_synced: StrictBool
    medicine_revision: StrictInt | None = Field(default=None,ge=1,le=1000000000)
    slots: list[MedicineSlot] | None = None

    @model_validator(mode='after')
    def check_slots(self):
        if self.schema_version == 2:
            if self.medicine_revision is None or self.slots is None or [s.slot for s in self.slots] != [0,1,2]:
                raise ValueError('schema 2 requires three ordered slots and a medicine revision')
            first=self.slots[0]
            if (first.enabled,first.minute,first.state,first.confirmed_epoch)!=(self.medicine_enabled,self.medicine_minute,self.medicine_state,self.confirmed_epoch):
                raise ValueError('legacy fields must match slot zero')
        return self

def mock_living_command(life, name, params, epoch, light, alarm):
    if not life:
        life={'schema':2,'config_revision':1,'night_auto':True,'night_on':False,'light_valid':True,'light_owner':'off','medicine_enabled':False,'medicine_minute':480,'medicine_state':0,'dose_day':-1,'confirmed_epoch':0,'time_synced':True,'medicine_revision':1}
    slots=life.get('slots') or [dict(slot=i,enabled=life['medicine_enabled'] if i==0 else False,minute=life['medicine_minute'] if i==0 else (720 if i==1 else 1200),state=0,confirmed_epoch=life['confirmed_epoch'] if i==0 else 0) for i in range(3)]
    life['schema']=2;life['medicine_revision']=life.get('medicine_revision') or 1
    day=(epoch+28800)//86400;minute=(epoch+28800)%86400//60
    for slot in slots:
        if slot['confirmed_epoch'] and (slot['confirmed_epoch']+28800)//86400!=day:slot['confirmed_epoch']=0
    result=0
    if name in ('set_living','set_medicine_plan'):
        if params['expected_revision']!=life['config_revision']:result=4
        else:
            life['night_auto']=params['night_auto'];life['config_revision']+=1
            for i in range(3 if name=='set_medicine_plan' else 1):
                suffix='' if i==0 else '_'+str(i+1)
                slots[i].update(enabled=params['medicine_enabled'+suffix],minute=params['medicine_minute'+suffix])
    else:
        i=params.get('slot',0);slot=slots[i];taken=params.get('taken',True)
        if name=='set_medicine_state' and params['expected_medicine_revision']!=life['medicine_revision']:result=4
        elif params['dose_day']!=day or not slot['enabled'] or taken and minute<slot['minute']:result=1
        elif bool(slot['confirmed_epoch'])!=taken:
            slot['confirmed_epoch']=epoch if taken else 0;life['medicine_revision']+=1
    for slot in slots:slot['state']=0 if not slot['enabled'] else 4 if slot['confirmed_epoch'] else 3 if minute>=slot['minute'] else 2
    pending=any(s['state']==3 for s in slots)
    life.update(slots=slots,dose_day=day,time_synced=True,medicine_enabled=slots[0]['enabled'],medicine_minute=slots[0]['minute'],medicine_state=slots[0]['state'],confirmed_epoch=slots[0]['confirmed_epoch'])
    life['night_on']=life['night_auto'] and not pending and light<30 and not alarm
    life['light_owner']='alarm' if alarm else 'medicine' if pending else 'night' if life['night_on'] else 'off'
    return life,result
