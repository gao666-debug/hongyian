import { textToSpeech } from '@kit.CoreSpeechKit';

// 语音播报：把报警和服药提醒念出来，看不清屏幕也能第一时间知道。
// 引擎不可用（模拟器或非华为设备）时静默降级为不播报，绝不阻塞主流程。
export class Voice {
  private static engine: textToSpeech.TextToSpeechEngine | null = null;
  private static preparing: boolean = false;
  private static unavailable: boolean = false;
  private static enabled: boolean = true;
  private static lastText: string = '';
  private static lastAt: number = 0;

  static setEnabled(on: boolean): void {
    Voice.enabled = on;
    if (!on) { Voice.stop(); }
  }

  static isEnabled(): boolean { return Voice.enabled; }

  static isUnavailable(): boolean { return Voice.unavailable; }

  static async prepare(): Promise<boolean> {
    if (Voice.engine) { return true; }
    if (Voice.unavailable || Voice.preparing) { return false; }
    Voice.preparing = true;
    try {
      const params: textToSpeech.CreateEngineParams = {
        language: 'zh-CN', person: 13, online: 1,
        extraParams: { 'style': 'interaction-broadcast', 'locate': 'CN', 'name': 'yihongan' }
      };
      const engine = await textToSpeech.createEngine(params);
      engine.setListener({
        onStart: (requestId: string, response: textToSpeech.StartResponse): void => { },
        onComplete: (requestId: string, response: textToSpeech.CompleteResponse): void => { },
        onStop: (requestId: string, response: textToSpeech.StopResponse): void => { },
        onError: (requestId: string, errorCode: number, errorMessage: string): void => { }
      });
      Voice.engine = engine;
      return true;
    } catch (error) {
      Voice.unavailable = true;
      return false;
    } finally {
      Voice.preparing = false;
    }
  }

  // urgent=true 打断正在播放的普通提醒；普通播报在忙时直接丢弃，避免排队堆积。
  static async speak(text: string, urgent: boolean = false): Promise<void> {
    const content = (text || '').trim();
    if (!Voice.enabled || !content) { return; }
    const now = Date.now();
    if (content === Voice.lastText && now - Voice.lastAt < 8000) { return; }
    if (!await Voice.prepare()) { return; }
    const engine = Voice.engine;
    if (!engine) { return; }
    try {
      if (engine.isBusy()) {
        if (!urgent) { return; }
        engine.stop();
      }
      Voice.lastText = content;
      Voice.lastAt = now;
      engine.speak(content, { requestId: 'y' + now });
    } catch (error) { /* 播报失败不影响业务 */ }
  }

  static stop(): void {
    const engine = Voice.engine;
    if (!engine) { return; }
    try { if (engine.isBusy()) { engine.stop(); } } catch (error) { }
  }

  static release(): void {
    const engine = Voice.engine;
    Voice.engine = null;
    if (!engine) { return; }
    try { engine.shutdown(); } catch (error) { }
  }
}
