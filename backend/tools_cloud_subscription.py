"""Create a device-scoped AMQP routing rule without rotating credentials."""
import json
import os

from backend.cloud import HuaweiCloud
from backend.settings import Settings, ROOT
from huaweicloudsdkiotda.v5 import (
    ListRoutingRulesRequest, CreateRoutingRuleRequest, AddRuleReq,
    RoutingRuleSubject, ListRuleActionsRequest, CreateRuleActionRequest,
    AddActionReq, ChannelDetail, AmqpForwarding, UpdateRoutingRuleRequest,
    UpdateRuleReq, BatchShowQueueRequest,
)


def main():
    settings = Settings.load()
    client = HuaweiCloud(settings).client
    queue = os.getenv('AMQP_QUEUE', '')
    if not queue:
        raise SystemExit('Set AMQP_QUEUE to an existing queue')
    queues = client.batch_show_queue(BatchShowQueueRequest()).to_dict().get('queues', [])
    if queue not in {item['queue_name'] for item in queues}:
        raise SystemExit('Configured AMQP queue does not exist')
    name = 'ZhianSafetyDeviceReports'
    rules = client.list_routing_rules(ListRoutingRulesRequest(rule_name=name)).to_dict().get('rules', [])
    own = [rule for rule in rules if rule['rule_name'] == name]
    if own:
        rule_id = own[0]['rule_id']
    else:
        rule = client.create_routing_rule(CreateRoutingRuleRequest(body=AddRuleReq(
            rule_name=name, description='Home Safety reports to local backend AMQP consumer',
            subject=RoutingRuleSubject(resource='device.message', event='report'),
            app_type='GLOBAL', select='*',
            where=f"notify_data.header.device_id = '{settings.device_id}'"))).to_dict()
        rule_id = rule['rule_id']
    actions = client.list_rule_actions(ListRuleActionsRequest(rule_id=rule_id)).to_dict().get('actions', [])
    if not any(action.get('channel') == 'AMQP_FORWARDING' and
               (action.get('channel_detail', {}).get('amqp_forwarding') or {}).get('queue_name') == queue
               for action in actions):
        client.create_rule_action(CreateRuleActionRequest(body=AddActionReq(
            rule_id=rule_id, channel='AMQP_FORWARDING',
            channel_detail=ChannelDetail(amqp_forwarding=AmqpForwarding(queue_name=queue)))))
    client.update_routing_rule(UpdateRoutingRuleRequest(rule_id=rule_id, body=UpdateRuleReq(active=True)))
    record = {'rule_id': rule_id, 'rule_name': name, 'queue': queue,
              'device_id': settings.device_id, 'active': True}
    (ROOT / '.local/cloud-subscription.json').write_text(json.dumps(record, indent=2))
    print(json.dumps(record))


if __name__ == '__main__':
    main()
