// 语音指令解析：先说唤醒词「小颐」，再说要做的事。
// 纯字符串逻辑，不依赖设备能力，便于测试与在无法录音时手动输入验证。
export interface VoiceIntent {
  type: string;
  reply: string;
  wake: boolean;
}

export class VoiceCommand {
  // 识别结果常把「颐」听成同音字，保留几个近似词，避免老人喊三遍没反应。
  static readonly WAKE: string[] = ['小颐', '小依', '小怡', '小宜'];

  static clean(raw: string): string {
    return (raw || '').replace(/[\s，。！？、,.!?~～…；;：:"'“”‘’]/g, '');
  }

  static hasWake(raw: string): boolean {
    const text = VoiceCommand.clean(raw);
    return VoiceCommand.WAKE.some((word: string): boolean => text.includes(word));
  }

  static stripWake(raw: string): string {
    let text = VoiceCommand.clean(raw);
    for (const word of VoiceCommand.WAKE) {
      text = text.split(word).join('');
    }
    return text;
  }

  static exampleList(): string[] {
    return ['小颐开灯', '小颐关灯', '小颐自动夜灯', '小颐救命', '小颐我吃过药了',
      '小颐现在温度多少', '小颐看报警', '小颐看趋势', '小颐看设备', '小颐打开摄像头'];
  }

  static parse(raw: string): VoiceIntent {
    const text = VoiceCommand.clean(raw);
    if (!text) { return { type: 'unknown', reply: '我没听清，请再说一次', wake: false }; }
    const wake = VoiceCommand.hasWake(text);
    const body = wake ? VoiceCommand.stripWake(text) : text;
    if (!body) { return { type: 'awake', reply: '我在，请说要做什么', wake: true }; }
    const hit = (pattern: RegExp, type: string, reply: string): VoiceIntent => {
      return { type: type, reply: reply, wake: wake };
    };
    // 顺序很关键：「自动夜灯」要先于「开灯」判断，避免被当成普通开灯。
    if (/自动/.test(body) && /灯|光|模式/.test(body)) { return hit(/./, 'light_auto', '好的，夜灯恢复自动'); }
    if (/开灯|打开灯|开夜灯|开个灯|灯打开|把灯打开|灯亮/.test(body)) { return hit(/./, 'light_on', '好的，开灯'); }
    if (/关灯|关闭灯|关夜灯|灯关掉|把灯关|灯灭/.test(body)) { return hit(/./, 'light_off', '好的，关灯'); }
    if (/救命|求助|呼救|帮帮我|需要帮助|救人|不舒服|快来|叫人/.test(body)) { return hit(/./, 'sos', '正在发起求助，请在弹窗里确认'); }
    if (/吃药|服药|药吃|吃过药|药吃了/.test(body)) { return hit(/./, 'medicine', '记下这次服药'); }
    if (/报警|警报|事件/.test(body) && /确认|知道|处理|收到|核实|没事/.test(body)) { return hit(/./, 'ack', '正在确认报警'); }
    if (/温度|多少度|湿度|潮不潮|冷不冷|热不热|环境/.test(body)) { return hit(/./, 'temperature', '正在读取当前温湿度'); }
    if (/报警|警报|事件|记录/.test(body)) { return hit(/./, 'alarm', '打开报警记录'); }
    if (/趋势|报告|活动|统计|规律/.test(body)) { return hit(/./, 'trend', '打开活动趋势'); }
    if (/设备|在线|电量|状态/.test(body)) { return hit(/./, 'device', '打开设备状态'); }
    if (/摄像头|画面|看看家|家里怎么样|视频/.test(body)) { return hit(/./, 'camera', '打开家中画面'); }
    if (/帮助|能做什么|怎么用|指令|会什么/.test(body)) { return hit(/./, 'help', '我能开灯关灯、发起求助、确认吃药、报温度、打开报警和趋势'); }
    return { type: 'unknown', reply: '这句话我还不熟，可以说：小颐开灯，或者小颐救命', wake: wake };
  }
}
