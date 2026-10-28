export interface CareInput {
  demo: boolean;
  online: boolean;
  fresh: boolean;
  healthFresh: boolean;
  environment: boolean;
  motion: boolean;
  alarmCode: number;
  activeAlarms: number;
}
export interface CareAssessment { level: string; title: string; reason: string; advice: string; }
export function assessCare(input: CareInput): CareAssessment {
  if (input.demo) {
    return { level: 'demo', title: '演示中的守护状态', reason: '当前数据仅用于界面体验', advice: '连接真实设备后，再查看实际监测状态与建议。' };
  }
  if (input.alarmCode > 0 || input.activeAlarms > 0) {
    return { level: 'alert', title: '有报警需要确认', reason: '设备状态或事件记录中仍有报警', advice: '先查看报警详情，联系现场人员核实，再记录处理结果；消音不代表现场异常已解除。' };
  }
  if (!input.online || !input.fresh || !input.healthFresh) {
    return { level: 'waiting', title: '监测状态待确认', reason: !input.online ? '设备离线或尚未确认在线' : !input.fresh ? '设备上报已过期或尚未收到' : '传感器健康信息尚未更新', advice: '检查开发板供电、热点及网络连接，等待新的数据与健康消息。没有数据不能视为一切正常。' };
  }
  if (!input.environment) {
    return { level: 'attention', title: '部分监测暂不可用', reason: '温湿度监测暂不可用', advice: '检查设备传感器状态；恢复前结合现场情况确认，不依赖缺失的监测项目。' };
  }
  return { level: 'normal', title: '当前未见报警', reason: '设备在线、上报及时，温湿度监测可用', advice: '固定主机监测居家环境，不判断人体跌倒。请定期联系家人；未见报警不等于确认人身安全。' };
}
