// 语音指令解析：先说唤醒词「小颐」，再说要做的事。
// 纯字符串逻辑，不依赖设备能力，便于测试与在无法录音时手动输入验证。
//
// 老人说话有三个特点，解析要跟着放宽：
//   1. 同音字：识别引擎常把「颐」写成依/怡/宜，把「灯」写成登/等；
//   2. 语序随意：「把灯给我关了」「灯打开一下」这种不含标准词的说法；
//   3. 中间夹语气词：「小颐啊，帮我看看报警」。
// 做法是先按同音字归一，再用关键词打分，最后才给「你是不是想说」的提示。
export interface VoiceIntent {
  type: string;
  reply: string;
  wake: boolean;
}

interface VoiceRule {
  type: string;
  reply: string;
  words: string[];
}

export class VoiceCommand {
  // 识别结果常把「颐」听成同音字，保留几个近似词，避免老人喊三遍没反应。
  static readonly WAKE: string[] = ['小颐', '小依', '小怡', '小宜', '小姨', '小仪'];

  // 同音字归一表：每组第一个字是代表字，其余是识别时容易写错的同音字。
  // 只收与指令相关、且不会把常用口语改坏的字；同一字只能出现在一组里。
  static readonly HOMO: string[] = [
    '颐依怡宜姨遗移',
    '灯登等蹬瞪凳',
    '关观官管馆惯',
    '救就旧九久酒',
    '命明名鸣冥铭',
    '药摇腰咬耀遥',
    '吃迟持池赤',
    '度渡肚杜',
    '温闻文稳',
    '湿诗师失施',
    '报抱保宝',
    '警井景静惊',
    '记技纪计季',
    '录路陆露鹿',
    '趋取曲娶',
    '势事市式试',
    '设射社舍',
    '备被背倍',
    '摄涉',
    '像象向相',
    '头投偷',
    '视是示',
    '频品贫',
    '夜业叶页',
    '自字子',
    '动东冬懂',
    '家加佳嘉',
    '里李理力礼',
    '确却缺',
    '认人任仁',
    '险先显鲜',
    '帮邦',
    '倒到道盗',
    '开凯'
  ];

  // 指令表：words 里的词按“出现即命中”计分，越长越具体、得分越高。
  // 顺序不影响结果，但自动夜灯要放在开灯前，避免「自动灯」被当成普通开灯。
  static readonly RULES: VoiceRule[] = [
    { type: 'light_auto', reply: '好的，夜灯恢复自动',
      words: ['自动夜灯', '夜灯自动', '灯光自动', '自动模式', '自动灯', '恢复自动', '自动亮'] },
    { type: 'light_off', reply: '好的，关灯',
      words: ['关灯', '关夜灯', '关闭灯', '关上灯', '把灯关了', '灯关掉', '灯灭了', '关一下灯', '灯关'] },
    { type: 'light_on', reply: '好的，开灯',
      words: ['开灯', '打开灯', '开夜灯', '把灯打开', '点灯', '亮灯', '开一下灯', '灯打开'] },
    { type: 'sos', reply: '正在发起求助，请在弹窗里确认',
      words: ['救命', '求助', '呼救', '求救', '救我', '求命', '帮帮我', '帮帮忙', '需要帮助', '救人', '不舒服',
        '快来', '叫人', '摔倒了', '摔了一跤', '摔跤', '起不来', '爬不起来', '晕倒', '昏倒', '不行了', '难受'] },
    { type: 'medicine', reply: '记下这次服药',
      words: ['吃药', '服药', '吃药了', '吃过药', '药吃了', '药吃过', '吃了药', '吃完药', '药吃完', '药已经吃'] },
    { type: 'temperature', reply: '正在读取当前温湿度',
      words: ['温度', '多少度', '几度', '湿度', '潮不潮', '冷不冷', '热不热', '环境'] },
    { type: 'alarm', reply: '打开报警记录',
      words: ['报警', '警报', '事件', '记录'] },
    { type: 'trend', reply: '打开活动趋势',
      words: ['趋势', '报告', '活动记录', '活动', '统计', '规律'] },
    { type: 'device', reply: '打开设备状态',
      words: ['设备', '在线', '电量', '状态'] },
    { type: 'camera', reply: '正在打开家中画面',
      words: ['摄像头', '画面', '看看家', '家里怎么样', '视频', '看家'] },
    { type: 'help', reply: '我可以说：开灯、关灯、自动夜灯、救命、我吃过药了、现在温度、看报警、看趋势、看设备、打开摄像头',
      words: ['帮助', '能做什么', '怎么用', '指令', '会什么', '你可以做什么'] }
  ];

  static clean(raw: string): string {
    return (raw || '').replace(/[\s，。！？、,.!?~～…；;：:"'“”‘’]/g, '');
  }

  // 同音字归一：把识别出来的错字换成代表字，后续比较就只看“像不像”。
  static canon(text: string): string {
    let out = '';
    for (let i = 0; i < text.length; i++) {
      const ch = text.charAt(i);
      let hit = ch;
      for (const group of VoiceCommand.HOMO) {
        if (group.indexOf(ch) > 0) { hit = group.charAt(0); break; }
      }
      out += hit;
    }
    return out;
  }

  static hasWake(raw: string): boolean {
    const text = VoiceCommand.clean(raw);
    return VoiceCommand.WAKE.some((word: string): boolean => text.indexOf(word) >= 0);
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

  // 编辑距离，用于「四字以上的说法差一个字」时仍能认出来。
  static distance(a: string, b: string): number {
    if (a === b) { return 0; }
    const n = a.length;
    const m = b.length;
    if (n === 0) { return m; }
    if (m === 0) { return n; }
    let prev: number[] = [];
    for (let j = 0; j <= m; j++) { prev.push(j); }
    for (let i = 1; i <= n; i++) {
      const cur: number[] = [i];
      for (let j = 1; j <= m; j++) {
        const cost = a.charAt(i - 1) === b.charAt(j - 1) ? 0 : 1;
        let best = prev[j] + 1;
        if (cur[j - 1] + 1 < best) { best = cur[j - 1] + 1; }
        if (prev[j - 1] + cost < best) { best = prev[j - 1] + cost; }
        cur.push(best);
      }
      prev = cur;
    }
    return prev[m];
  }

  // 窗口内最近似距离：只要句子某一段与词差一个字就算像。
  private static windowDistance(text: string, word: string): number {
    const len = word.length;
    if (len === 0 || text.length < len) { return len; }
    let best = len;
    for (let i = 0; i + len <= text.length; i++) {
      const d = VoiceCommand.distance(text.substr(i, len), word);
      if (d < best) { best = d; }
      if (best === 0) { break; }
    }
    return best;
  }

  // 单词得分：命中越具体分越高；四字以上允许差一个字（老人常吞字）。
  private static score(text: string, word: string): number {
    const w = VoiceCommand.canon(word);
    if (!w) { return 0; }
    if (text.indexOf(w) >= 0) { return 100 + w.length * 2; }
    if (w.length >= 4 && VoiceCommand.windowDistance(text, w) <= 1) { return 50 + w.length; }
    return 0;
  }

  // 给「没听懂」时用的候选建议，只提示、不执行，但也不能乱猜：
  // 两字词差一个字就等于换了意思（「关灯」/「关掉」），所以只对三字以上给建议。
  private static suggest(text: string): string {
    let best = '';
    let bestScore = 999;
    for (const rule of VoiceCommand.RULES) {
      for (const word of rule.words) {
        const w = VoiceCommand.canon(word);
        if (w.length < 3) { continue; }
        const d = VoiceCommand.windowDistance(text, w);
        const limit = w.length >= 5 ? 2 : 1;
        if (d > limit) { continue; }
        // 距离相同时优先更长的词，命中更具体。
        if (d < bestScore || (d === bestScore && w.length > best.length)) { bestScore = d; best = word; }
      }
    }
    return best;
  }

  static parse(raw: string): VoiceIntent {
    const text = VoiceCommand.clean(raw);
    if (!text) { return { type: 'unknown', reply: '我没听清，请再说一次', wake: false }; }
    const wake = VoiceCommand.hasWake(text);
    const body = VoiceCommand.stripWake(text);
    if (!body) { return { type: 'awake', reply: '我在，请说要做什么', wake: true }; }

    // 归一后再比较：两边的同音字被换成同一个字，错字也能对上。
    const norm = VoiceCommand.canon(body);

    // 「确认报警」要先于普通「看报警」判断，否则会被当成打开列表。
    const isAlarm = norm.indexOf(VoiceCommand.canon('报警')) >= 0 || norm.indexOf(VoiceCommand.canon('警报')) >= 0 ||
      norm.indexOf(VoiceCommand.canon('事件')) >= 0;
    const isAck = ['确认', '知道', '处理', '收到', '核实', '没事', '别叫', '取消'].some(
      (w: string): boolean => norm.indexOf(VoiceCommand.canon(w)) >= 0);
    if (isAlarm && isAck) { return { type: 'ack', reply: '正在确认报警', wake: wake }; }

    // 提到灯又提到开关，语序再乱也能判定，覆盖「把灯给我关了」这类说法。
    if (norm.indexOf('灯') >= 0) {
      if (norm.indexOf('自动') >= 0) { return { type: 'light_auto', reply: '好的，夜灯恢复自动', wake: wake }; }
      if (norm.indexOf('关') >= 0 && norm.indexOf('开') < 0) { return { type: 'light_off', reply: '好的，关灯', wake: wake }; }
      if (norm.indexOf('开') >= 0 || norm.indexOf('亮') >= 0) { return { type: 'light_on', reply: '好的，开灯', wake: wake }; }
    }

    let hitType = '';
    let hitReply = '';
    let hitScore = 0;
    for (const rule of VoiceCommand.RULES) {
      for (const word of rule.words) {
        const s = VoiceCommand.score(norm, word);
        if (s > hitScore) { hitScore = s; hitType = rule.type; hitReply = rule.reply; }
      }
    }
    if (hitType) { return { type: hitType, reply: hitReply, wake: wake }; }

    const guess = VoiceCommand.suggest(norm);
    if (guess) {
      return { type: 'unknown', reply: '我没听清，你是不是想说「小颐' + guess + '」？也可以直接点上面的按钮', wake: wake };
    }
    return { type: 'unknown', reply: '这句话我还不熟，可以说：小颐开灯，或者小颐救命', wake: wake };
  }
}
