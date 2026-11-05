import { speechRecognizer } from '@kit.CoreSpeechKit';

// 语音识别：点一下麦克风说一句，识别结果交给指令解析。
// 设备不支持（模拟器、无麦克风权限、引擎缺失）时返回 false，由页面给出手动输入兜底。
export class VoiceListen {
  private static engine: speechRecognizer.SpeechRecognitionEngine | null = null;
  private static unavailable: boolean = false;
  private static busy: boolean = false;
  private static session: string = '';
  private static said: string = '';
  private static onText: (text: string, final: boolean) => void = (text: string, final: boolean): void => { };
  private static onState: (message: string) => void = (message: string): void => { };
  private static onDone: (message: string) => void = (message: string): void => { };

  static isUnavailable(): boolean { return VoiceListen.unavailable; }

  static isBusy(): boolean { return VoiceListen.busy; }

  private static listener(): speechRecognizer.RecognitionListener {
    return {
      onStart: (sessionId: string, eventMessage: string): void => {
        VoiceListen.said = '';
        VoiceListen.onState('我在听，请说');
      },
      onEvent: (sessionId: string, eventCode: number, eventMessage: string): void => { },
      onResult: (sessionId: string, result: speechRecognizer.SpeechRecognitionResult): void => {
        const text = (result.result || '').trim();
        if (text) {
          VoiceListen.said = text;
          VoiceListen.onText(text, !!(result.isFinal || result.isLast));
        }
      },
      onComplete: (sessionId: string, eventMessage: string): void => {
        VoiceListen.busy = false;
        VoiceListen.onDone(VoiceListen.said ? '' : '没听清，请再说一次');
      },
      onError: (sessionId: string, errorCode: number, errorMessage: string): void => {
        VoiceListen.busy = false;
        VoiceListen.onDone('语音识别没能完成（' + errorCode + '），可以手动输入指令');
      }
    };
  }

  static async prepare(): Promise<boolean> {
    if (VoiceListen.engine) { return true; }
    if (VoiceListen.unavailable) { return false; }
    try {
      const params: speechRecognizer.CreateEngineParams = { language: 'zh-CN', online: 1 };
      const engine = await speechRecognizer.createEngine(params);
      engine.setListener(VoiceListen.listener());
      VoiceListen.engine = engine;
      return true;
    } catch (error) {
      VoiceListen.unavailable = true;
      return false;
    }
  }

  static async start(onText: (text: string, final: boolean) => void,
    onState: (message: string) => void, onDone: (message: string) => void): Promise<boolean> {
    if (VoiceListen.busy) { return true; }
    if (!await VoiceListen.prepare()) { return false; }
    const engine = VoiceListen.engine;
    if (!engine) { return false; }
    VoiceListen.onText = onText;
    VoiceListen.onState = onState;
    VoiceListen.onDone = onDone;
    VoiceListen.said = '';
    const session = 'yihongan' + Date.now();
    VoiceListen.session = session;
    try {
      VoiceListen.busy = true;
      engine.startListening({
        sessionId: session,
        audioInfo: { audioType: 'pcm', sampleRate: 16000, soundChannel: 1, sampleBit: 16 },
        extraParams: { 'recognitionMode': 0, 'vadBegin': 3000, 'vadEnd': 1800, 'maxAudioDuration': 20000 }
      });
      return true;
    } catch (error) {
      VoiceListen.busy = false;
      VoiceListen.unavailable = true;
      return false;
    }
  }

  static stop(): void {
    const engine = VoiceListen.engine;
    if (!engine || !VoiceListen.session) { return; }
    try { engine.finish(VoiceListen.session); } catch (error) { VoiceListen.busy = false; }
  }

  static cancel(): void {
    const engine = VoiceListen.engine;
    VoiceListen.busy = false;
    if (!engine || !VoiceListen.session) { return; }
    try { engine.cancel(VoiceListen.session); } catch (error) { }
  }

  static release(): void {
    VoiceListen.cancel();
    const engine = VoiceListen.engine;
    VoiceListen.engine = null;
    if (!engine) { return; }
    try { engine.shutdown(); } catch (error) { }
  }
}
