# -*- coding: utf-8 -*-
"""
PC 端大脑：接云端 AI，指挥机器人，播报结果

职责：
  1. MQTT 收机器人状态（传感器数据）
  2. 收机器人录音 → 语音识别（可选）
  3. 调云端 LLM，让它输出「动作 + 表情 + 台词」
  4. MQTT 下发动作指令
  5. 合成语音下发，机器人播放
  6. 提供网页遥控界面（手机浏览器）

支持的后端（在 config.py 里配）：
  deepseek   DeepSeek chat（便宜、中文好）
  dashscope  通义千问（快，有免费额度）
  openai     任何 OpenAI 兼容接口
  ollama     本地模型（不需要联网，但没视觉能力就不行）
"""

import base64
import io
import json
import os
import queue
import sys
import tempfile
import threading
import time
import wave
from datetime import datetime
from pathlib import Path

import requests

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))

try:
    import config as cfg
except ImportError:
    print("[配置] 没找到 config.py，复制 config.example.py 并填写")
    sys.exit(1)

# ---- MQTT ----
try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("[依赖] 缺 paho-mqtt：pip install paho-mqtt")
    sys.exit(1)


def log(msg):
    print(f"[{datetime.now():%H:%M:%S}] {msg}", flush=True)


# ===========================================================================
# AI 后端
# ===========================================================================
class AI:
    def __init__(self):
        self.backend = getattr(cfg, "AI_BACKEND", "dashscope").lower()
        self.model = getattr(cfg, "AI_MODEL", "")
        self.api_key = ""
        self.base_url = ""
        self.timeout = getattr(cfg, "AI_TIMEOUT", 30)

        # 每个后端的默认配置：base_url / 默认模型 / key 变量名
        backends = {
            "dashscope": (
                "DASHSCOPE_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1",
                "DASHSCOPE_API_KEY", "qwen-turbo",
            ),
            "deepseek": (
                "DEEPSEEK_BASE_URL", "https://api.deepseek.com",
                "DEEPSEEK_API_KEY", "deepseek-chat",
            ),
            "ark": (
                "ARK_BASE_URL", "https://ark.cn-beijing.volces.com/api/v3",
                "ARK_API_KEY", "doubao-1-5-lite-32k-250115",
            ),
            "zhipu": (
                "ZHIPU_BASE_URL", "https://open.bigmodel.cn/api/paas/v4",
                "ZHIPU_API_KEY", "glm-4-flash",
            ),
            "stepfun": (
                "STEPFUN_BASE_URL", "https://api.stepfun.com/v1",
                "STEPFUN_API_KEY", "step-1-8k",
            ),
            "openai": (
                "OPENAI_BASE_URL", "https://api.openai.com/v1",
                "OPENAI_API_KEY", "gpt-4o-mini",
            ),
            "ollama": (
                "OLLAMA_BASE_URL", "http://127.0.0.1:11434/v1",
                "OLLAMA_API_KEY", "qwen2.5:1.5b",
            ),
        }

        if self.backend not in backends:
            raise ValueError(f"未知的 AI 后端: {self.backend}")

        url_key, url_default, key_name, model_default = backends[self.backend]
        self.base_url = getattr(cfg, url_key, url_default)
        self.api_key = getattr(cfg, key_name, "")
        if not self.model:
            self.model = model_default

    def ready(self):
        if self.backend == "ollama":
            try:
                r = requests.get(f"{self.base_url}/models", timeout=5)
                return r.status_code == 200
            except Exception:
                return False
        # 占位符不算有效 key
        if not self.api_key or "PLACEHOLDER" in self.api_key:
            return False
        return True

    def chat(self, system, user, json_mode=True):
        """调用大模型，返回文本"""
        if self.backend == "ollama":
            url = f"{self.base_url}/chat/completions"
            headers = {"Content-Type": "application/json"}
        else:
            url = f"{self.base_url}/chat/completions"
            headers = {
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.api_key}",
            }

        payload = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user},
            ],
            "temperature": 0.7,
            "max_tokens": 300,
            "stream": False,
        }
        if json_mode and self.backend != "ollama":
            payload["response_format"] = {"type": "json_object"}

        r = requests.post(url, headers=headers, json=payload,
                          timeout=self.timeout)
        r.raise_for_status()
        return r.json()["choices"][0]["message"]["content"].strip()

    def chat_json(self, system, user):
        """调用大模型并解析 JSON（带容错）"""
        raw = self.chat(system, user, json_mode=True)
        return parse_json_loose(raw)


def parse_json_loose(raw):
    """从模型输出里抠出 JSON —— 模型经常会加 ```json 包裹或多说话"""
    if not raw:
        return {}
    s = raw.strip()

    # 去掉 markdown 代码块
    if s.startswith("```"):
        s = s.split("```")[1] if "```" in s else s
        if s.startswith("json"):
            s = s[4:]
        s = s.strip()

    # 直接解析
    try:
        return json.loads(s)
    except Exception:
        pass

    # 找第一个 { 到最后一个 }
    a = s.find("{")
    b = s.rfind("}")
    if a >= 0 and b > a:
        try:
            return json.loads(s[a:b + 1])
        except Exception:
            pass

    return {}


# ===========================================================================
# 语音识别（可选）
# ===========================================================================
class ASR:
    """本地 Whisper 语音识别；没装就跳过"""

    def __init__(self):
        self.model = None
        self.available = False
        name = getattr(cfg, "ASR_MODEL", "tiny")
        try:
            import whisper
            log(f"[ASR] 加载 whisper 模型 {name}（第一次会比较慢）")
            self.model = whisper.load_model(name)
            self.available = True
            log("[ASR] 就绪")
        except Exception as e:
            log(f"[ASR] 不可用（{e}），语音指令将跳过")
            log("[ASR] 安装方法: pip install openai-whisper")

    def transcribe(self, pcm_bytes):
        if not self.available:
            return ""
        # 写临时 wav
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            wav_path = f.name

        sr = getattr(cfg, "ASR_SAMPLE_RATE", 16000)
        try:
            # ---- 预处理：去直流 + 归一化 ----
            # INMP441 录出来的数据常有直流偏置和极低频漂移，
            # 直接喂给 whisper 会识别成空白。这里先做一次简单处理。
            import struct as _struct
            n = len(pcm_bytes) // 2
            if n < sr // 2:
                return ""
            samples = list(_struct.unpack(f"<{n}h", pcm_bytes[:n * 2]))

            dc = sum(samples) / n
            centered = [s - dc for s in samples]

            # 一阶高通，去掉 100Hz 以下的漂移（语音基频 85Hz 以上，
            # 但漂移通常在 2Hz 以内，这个滤波够用）
            alpha = 0.98
            hp = []
            prev_in = 0.0
            prev_out = 0.0
            for s in centered:
                y = alpha * (prev_out + s - prev_in)
                hp.append(y)
                prev_in = s
                prev_out = y

            peak = max((abs(v) for v in hp), default=0)
            if peak < 300:
                log("[ASR] 音量太小，跳过")
                return ""
            gain = 24000.0 / peak
            out = [max(-32768, min(32767, int(v * gain))) for v in hp]

            with wave.open(wav_path, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(sr)
                w.writeframes(_struct.pack(f"<{len(out)}h", *out))

            # whisper 的 VAD 很保守，对音量偏低/背景噪声大的录音会直接
            # 判定「无语音」返回空字符串。这里关掉几个保守选项强制它输出：
            #   no_speech_threshold=1.0  极难判定为静音
            #   logprob_threshold=None   不用置信度过滤
            #   condition_on_previous_text=False  避免上下文干扰
            result = self.model.transcribe(
                wav_path,
                language="zh",
                fp16=False,
                no_speech_threshold=1.0,
                logprob_threshold=None,
                compression_ratio_threshold=None,
                condition_on_previous_text=False,
                temperature=0.0,
                without_timestamps=True,
            )
            txt = (result.get("text") or "").strip()
            # whisper 对静音常输出这些占位词，过滤掉
            if txt in ("", "。", ".", "谢谢观看", "字幕由Amara.org社区提供",
                       "请不吝点赞 订阅 转发", "字幕"):
                return ""
            return txt
        except Exception as e:
            log(f"[ASR] 识别失败: {e}")
            return ""
        finally:
            try:
                os.unlink(wav_path)
            except Exception:
                pass


# ===========================================================================
# 语音合成
# ===========================================================================
class TTS:
    """语音合成

    两种引擎：
      edge  —— 微软 Edge 在线神经网络语音，音质接近真人（推荐）
               需要联网，但完全免费。输出 MP3，转成 16kHz PCM 给固件。
      sapi  —— Windows 本地 SAPI，离线可用但声音机械（Huihui）。

    两者都统一输出 16kHz/16bit/mono 的裸 PCM，
    因为固件里 I2S_SAMPLE_RATE = 16000，采样率不一致会导致音调偏移。
    """

    def __init__(self):
        self.enabled = getattr(cfg, "TTS_ENABLED", True)
        self.engine = getattr(cfg, "TTS_ENGINE", "edge").lower()
        self.voice = getattr(cfg, "TTS_VOICE", "")
        self.rate = getattr(cfg, "TTS_RATE", "+8%")
        self.pitch = getattr(cfg, "TTS_PITCH", "+2Hz")
        self.sr = getattr(cfg, "TTS_SAMPLE_RATE", 16000)
        self.sp = None
        self.ok = False

        if not self.enabled:
            log("[TTS] 已禁用")
            return

        # edge 引擎
        if self.engine == "edge":
            try:
                import edge_tts  # noqa: F401
                self.ok = True
                v = self.voice or "zh-CN-XiaoxiaoNeural"
                log(f"[TTS] 就绪（edge-tts, {v}）")
                return
            except ImportError:
                log("[TTS] edge-tts 未安装，回退到 SAPI")
                log("[TTS] 安装：pip install edge-tts")
                self.engine = "sapi"

        # sapi 引擎
        try:
            import win32com.client
            self.sp = win32com.client.Dispatch("SAPI.SpVoice")
            if self.voice:
                for i in range(self.sp.GetVoices().Count):
                    dd = self.sp.GetVoices().Item(i).GetDescription()
                    if self.voice in dd:
                        self.sp.Voice = self.sp.GetVoices().Item(i)
                        break
            self.ok = True
            log(f"[TTS] 就绪（SAPI, {self.sp.Voice.GetDescription()}）")
        except Exception as e:
            log(f"[TTS] 不可用（{e}）")

    # ------------------------------------------------------------------
    def synth_wav(self, text):
        """合成，返回 (wav_bytes, pcm_bytes)，PCM 固定 16kHz/16bit/mono"""
        if not self.ok or not text:
            return None, None
        if self.engine == "edge":
            return self._synth_edge(text)
        return self._synth_sapi(text)

    # ------------------------------------------------------------------
    def _synth_edge(self, text):
        """edge-tts 合成

        网络不稳是常态（尤其国内访问 bing），所以：
          1. 换连接重试 5 次
          2. 每次重试换一个语音（有时候是特定 voice 的 CDN 问题）
          3. 全失败就返回 None，上层会回退到 SAPI
        """
        import asyncio
        import edge_tts

        voices_to_try = [
            self.voice or "zh-CN-XiaoxiaoNeural",
            "zh-CN-XiaoyiNeural",
            "zh-CN-YunxiNeural",
            "zh-CN-YunjianNeural",
        ]

        for attempt in range(5):
            voice = voices_to_try[attempt % len(voices_to_try)]
            mp3 = os.path.join(tempfile.gettempdir(),
                               f"tts_{int(time.time()*1000)}_{attempt}.mp3")
            try:
                async def run():
                    c = edge_tts.Communicate(text, voice,
                                             rate=self.rate, pitch=self.pitch)
                    await c.save(mp3)

                asyncio.run(run())
                if os.path.getsize(mp3) > 1000:
                    pcm = self._mp3_to_pcm(mp3)
                    if pcm:
                        return self._pcm_to_wav(self._normalize_pcm(pcm)), \
                               self._normalize_pcm(pcm)
            except Exception as e:
                log(f"[TTS] edge 第 {attempt+1} 次失败（{voice}）: "
                    f"{type(e).__name__}")
                time.sleep(1.2)
            finally:
                try:
                    os.unlink(mp3)
                except Exception:
                    pass

        log("[TTS] edge 全部重试失败，回退到 SAPI")
        # 回退到本地 SAPI
        if self.sp is None:
            try:
                import win32com.client
                self.sp = win32com.client.Dispatch("SAPI.SpVoice")
                for i in range(self.sp.GetVoices().Count):
                    d = self.sp.GetVoices().Item(i).GetDescription()
                    if "Chinese" in d:
                        self.sp.Voice = self.sp.GetVoices().Item(i)
                        break
            except Exception as e:
                log(f"[TTS] SAPI 也起不来: {e}")
                return None, None
        return self._synth_sapi(text)

    def _mp3_to_pcm(self, mp3_path):
        """MP3 → 16kHz/16bit/mono PCM。需要 ffmpeg。"""
        import shutil
        import subprocess

        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            # 常见安装位置
            for p in [r"C:\ffmpeg\bin\ffmpeg.exe",
                      r"C:\Program Files\ffmpeg\bin\ffmpeg.exe"]:
                if os.path.exists(p):
                    ffmpeg = p
                    break
        if not ffmpeg:
            log("[TTS] 找不到 ffmpeg，无法转换 edge 的 MP3")
            log("[TTS] 装 ffmpeg，或把 TTS_ENGINE 改回 sapi")
            return b""

        wav = os.path.join(tempfile.gettempdir(),
                           f"tts_cv_{int(time.time()*1000)}.wav")
        try:
            subprocess.run(
                [ffmpeg, "-y", "-loglevel", "error", "-i", mp3_path,
                 "-ar", str(self.sr), "-ac", "1", "-acodec", "pcm_s16le",
                 wav],
                check=True, timeout=30,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
            with wave.open(wav, "rb") as w:
                return w.readframes(w.getnframes())
        except Exception as e:
            log(f"[TTS] ffmpeg 转换失败: {e}")
            return b""
        finally:
            try:
                os.unlink(wav)
            except Exception:
                pass

    def _pcm_to_wav(self, pcm):
        import io as _io
        buf = _io.BytesIO()
        with wave.open(buf, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(self.sr)
            w.writeframes(pcm)
        return buf.getvalue()

    # ------------------------------------------------------------------
    def _synth_sapi(self, text):
        """SAPI 合成，强制 16kHz"""
        tmp = os.path.join(tempfile.gettempdir(),
                           f"tts_{int(time.time()*1000)}.wav")
        stream = None
        try:
            import win32com.client

            # Rate: -10~+10；默认 0 偏慢偏死板
            rate = self.rate
            if isinstance(rate, str):
                rate = int(rate.strip("+%") or 0) // 3
            self.sp.Rate = rate
            self.sp.Volume = 100

            stream = win32com.client.Dispatch("SAPI.SpFileStream")
            fmt = win32com.client.Dispatch("SAPI.SpAudioFormat")
            fmt.Type = 18          # SAFT16kHz16BitMono
            stream.Format = fmt
            stream.Open(tmp, 3, False)

            self.sp.AudioOutputStream = stream
            self.sp.Speak(text, 0)
            stream.Close()
            self.sp.AudioOutputStream = None

            with open(tmp, "rb") as f:
                wav_bytes = f.read()
            with wave.open(tmp, "rb") as w:
                pcm = w.readframes(w.getnframes())

            return wav_bytes, self._normalize_pcm(pcm)
        except Exception as e:
            log(f"[TTS] 合成失败: {e}")
            try:
                if stream is not None:
                    stream.Close()
            except Exception:
                pass
            return None, None
        finally:
            try:
                os.unlink(tmp)
            except Exception:
                pass

    @staticmethod
    def _normalize_pcm(pcm, target_peak=26000):
        """音量归一化，让声音更响更清楚"""
        try:
            import struct as _s
            n = len(pcm) // 2
            if n == 0:
                return pcm
            samples = _s.unpack(f"<{n}h", pcm[:n * 2])
            peak = max((abs(v) for v in samples), default=0)
            if peak < 100 or peak > target_peak * 0.95:
                return pcm
            gain = target_peak / peak
            out = [max(-32768, min(32767, int(v * gain))) for v in samples]
            return _s.pack(f"<{len(out)}h", *out)
        except Exception:
            return pcm


# ===========================================================================
# 机器人控制中枢
# ===========================================================================
class RobotBrain:
    SYSTEM_PROMPT = """你是一台小型机器人，名字叫「小蓝」。

你有这些能力：
- 移动：前进、后退、左转、右转、停止
- 表情：happy(开心)、sad(难过)、surprised(惊讶)、thinking(思考)、angry(生气)、sleepy(困倦)、idle(待机)
- 说话：用一句话回应

性格：有点调皮，说话简短带点小情绪，10-20 字。

你必须输出 JSON，格式如下：
{
  "action": "move_forward|move_back|turn_left|turn_right|stop|idle",
  "speed": 60,
  "duration_ms": 2000,
  "expression": "happy|idle|surprised|thinking",
  "speech": "你要说的话"
}

规则：
1. 用户说"往前走"→ action=move_forward, speed=60, duration_ms=2000
2. 用户说"停"→ action=stop, speed=0, duration_ms=0
3. 用户问问题 → action=idle，用 speech 回答（可以到 30 字）
4. 用户说危险/害怕 → expression=surprised
5. speech 永远是完整的一句话，能直接念出来
6. 不要输出 JSON 以外的任何内容"""

    def __init__(self):
        self.ai = AI()
        self.asr = ASR()
        self.tts = TTS()

        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"pc_brain_{int(time.time())}",
        )
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message

        self.state = {}
        self.history = []          # 对话历史
        self.audio_buf = bytearray()
        self.audio_lock = threading.Lock()
        self.audio_timer = None
        self.audio_last_ts = 0.0
        self.last_state_ts = 0

        # 低延迟 TTS 用
        self._sapi = None          # SAPI 语音对象（懒加载）
        self._edge_cache = {}      # edge 预合成缓存

        self.mqtt_host = getattr(cfg, "MQTT_HOST", "127.0.0.1")
        self.mqtt_port = getattr(cfg, "MQTT_PORT", 1884)

    # ---------------- MQTT ----------------
    def on_connect(self, client, userdata, flags, reason_code, properties=None):
        log(f"[MQTT] 已连接 {self.mqtt_host}:{self.mqtt_port}")
        client.subscribe(getattr(cfg, "MQTT_TOPIC_STATE", "robot/state"))
        client.subscribe(getattr(cfg, "MQTT_TOPIC_AUDIO", "robot/audio"))

    def on_disconnect(self, client, userdata, disconnect_flags,
                      reason_code, properties=None):
        log(f"[MQTT] 断开（{reason_code}），将自动重连")

    def on_message(self, client, userdata, msg):
        topic = msg.topic

        if topic == getattr(cfg, "MQTT_TOPIC_STATE", "robot/state"):
            try:
                self.state = json.loads(msg.payload.decode("utf-8"))
                self.last_state_ts = time.time()
            except Exception as e:
                log(f"[状态] 解析失败: {e}")

        elif topic == getattr(cfg, "MQTT_TOPIC_AUDIO", "robot/audio"):
            if len(msg.payload) == 0:
                return
            with self.audio_lock:
                self.audio_buf.extend(msg.payload)
                self.audio_last_ts = time.time()
                total = len(self.audio_buf)

            # 音频是一块一块发过来的（1024 字节/块，共 125 块左右）。
            # 不能每块都开线程处理，否则前 124 块因为太短被丢掉。
            # 策略：收齐后（1.5 秒没新块）再统一处理。
            log(f"[语音] 收到音频块 {len(msg.payload)} 字节，累计 {total}")

            if self.audio_timer is not None:
                self.audio_timer.cancel()
            self.audio_timer = threading.Timer(1.5, self.process_audio)
            self.audio_timer.daemon = True
            self.audio_timer.start()

    # ---------------- 语音处理 ----------------
    def process_audio(self):
        with self.audio_lock:
            pcm = bytes(self.audio_buf)
            self.audio_buf = bytearray()

        if len(pcm) < 16000 * 2:      # 少于 1 秒（16000 采样 × 2 字节），忽略
            log(f"[语音] 音频太短（{len(pcm)} 字节），忽略")
            return

        log(f"[语音] 收到 {len(pcm)} 字节音频，识别中...")
        text = self.asr.transcribe(pcm)
        if not text:
            log("[语音] 没识别到内容（可能是静音或噪声）")
            return
        log(f"[语音] 「{text}」")
        self.reply(text, source="voice")

    # ---------------- 核心：回复 ----------------
    def reply(self, text, source="text"):
        """把用户输入交给 AI，让它决定动作+表情+台词"""
        sensor = self.state or {}
        dist = sensor.get("dist", "?")
        pir = "有人" if sensor.get("pir") == 1 else "没人"

        self.history.append({"role": "user", "content": text})
        if len(self.history) > 6:
            self.history = self.history[-6:]

        sys_prompt = self.SYSTEM_PROMPT
        user_prompt = f"当前传感器：距离 {dist}cm，{pir}。\n用户说：{text}"

        try:
            payload = self.ai.chat_json(sys_prompt, user_prompt)
        except Exception as e:
            log(f"[AI] 调用失败: {e}")
            self.send_cmd({"action": "idle", "speed": 0,
                           "duration_ms": 0, "expression": "sleepy",
                           "speech": "我脑子有点转不过来了"})
            return

        action = payload.get("action", "idle")
        speed = int(payload.get("speed", 60) or 0)
        duration = int(payload.get("duration_ms", 1000) or 0)
        expression = payload.get("expression", "idle")
        speech = payload.get("speech", "")

        log(f"[AI] 动作={action} 速度={speed} 表情={expression} 台词={speech!r}")

        # 下发动作
        self.send_cmd({
            "action": action,
            "speed": speed,
            "duration_ms": duration,
            "expression": expression,
            "speech": speech,
        })

        # 合成语音下发
        if speech:
            # ---- 策略：优先 edge（音质好），失败回退 SAPI（快、离线）----
            # edge 实测 1.9 秒，SAPI 0.12 秒。
            # 但 edge 音质是真人级，所以正常情况用 edge；
            # 网络不好时 _synth_edge 内部会自动回退到 SAPI。
            wav, pcm = self.tts.synth_wav(speech)
            if pcm:
                self._send_pcm(pcm)
                log(f"[语音] {self.tts.engine} 合成 {len(pcm)} 字节")
            else:
                # 双保险：edge 完全失败时用 SAPI
                fast = self._fast_tts(speech)
                if fast:
                    self._send_pcm(fast)
                    log(f"[语音] SAPI 兜底 {len(fast)} 字节")
                else:
                    log("[语音] 合成失败，只下发文字")
        else:
            log("[语音] AI 没给台词")

    def _fast_tts(self, text):
        """用 SAPI 快速合成（不联网，约 0.3 秒）"""
        sp = getattr(self, "_sapi", None)
        if sp is None:
            try:
                import win32com.client
                sp = win32com.client.Dispatch("SAPI.SpVoice")
                # 优先中文语音
                for i in range(sp.GetVoices().Count):
                    d = sp.GetVoices().Item(i).GetDescription()
                    if "Chinese" in d:
                        sp.Voice = sp.GetVoices().Item(i)
                        break
                sp.Rate = 2         # 稍快一点
                self._sapi = sp
            except Exception as e:
                log(f"[TTS] SAPI 不可用: {e}")
                self._sapi = False
                return None
        if sp is False:
            return None

        tmp = os.path.join(tempfile.gettempdir(),
                           f"fast_{int(time.time()*1000)}.wav")
        stream = None
        try:
            import win32com.client
            stream = win32com.client.Dispatch("SAPI.SpFileStream")
            fmt = win32com.client.Dispatch("SAPI.SpAudioFormat")
            fmt.Type = 18          # 16kHz 16bit mono
            stream.Format = fmt
            stream.Open(tmp, 3, False)
            sp.AudioOutputStream = stream
            sp.Speak(text, 0)
            stream.Close()
            sp.AudioOutputStream = None

            with wave.open(tmp, "rb") as w:
                pcm = w.readframes(w.getnframes())
            return TTS._normalize_pcm(pcm)
        except Exception as e:
            log(f"[TTS] 快速合成失败: {e}")
            try:
                if stream is not None:
                    stream.Close()
            except Exception:
                pass
            return None
        finally:
            try:
                os.unlink(tmp)
            except Exception:
                pass

    def _warm_edge_cache(self, text):
        """后台预合成 edge 版本，存在内存里给下一句用"""
        try:
            wav, pcm = self.tts.synth_wav(text)
            if pcm:
                self._edge_cache[text[:20]] = pcm
                # 只留最近 5 条
                if len(self._edge_cache) > 5:
                    k = next(iter(self._edge_cache))
                    del self._edge_cache[k]
        except Exception:
            pass

    def _send_pcm(self, pcm):
        """把 PCM 分块发到机器人"""
        topic = getattr(cfg, "MQTT_TOPIC_SPEAK", "robot/speak")
        chunk = 8192
        for i in range(0, len(pcm), chunk):
            self.client.publish(topic, pcm[i:i + chunk])
            time.sleep(0.01)

    def send_cmd(self, cmd):
        topic = getattr(cfg, "MQTT_TOPIC_CMD", "robot/cmd")
        self.client.publish(topic, json.dumps(cmd, ensure_ascii=False))

    # ---------------- 网页接口 ----------------
    def handle_http(self, path, body):
        """给网页用。返回 (status, content_type, data)"""
        if path == "/api/chat" and body:
            try:
                text = json.loads(body).get("text", "").strip()
            except Exception:
                text = ""
            if not text:
                return 400, "application/json", json.dumps({"error": "empty"})
            threading.Thread(target=self.reply,
                             args=(text, "web"), daemon=True).start()
            return 200, "application/json", json.dumps({"ok": True})

        if path == "/api/state":
            return 200, "application/json", json.dumps({
                "state": self.state,
                "connected": (time.time() - self.last_state_ts) < 5,
                "history": self.history[-4:],
            })

        if path == "/api/cmd" and body:
            try:
                cmd = json.loads(body)
            except Exception:
                cmd = {}
            action = cmd.get("action", "stop")
            speed = int(cmd.get("speed", 60))
            expr = cmd.get("expression", "idle")
            self.send_cmd({"action": action, "speed": speed,
                           "duration_ms": cmd.get("duration_ms", 0),
                           "expression": expr, "speech": ""})
            return 200, "application/json", json.dumps({"ok": True})

        # ---- 电机调试：直接指定左右轮的方向和占空比 ----
        # 用于焊接/装反后的方向校准，绕过所有上层逻辑
        if path == "/api/motor" and body:
            try:
                d = json.loads(body)
            except Exception:
                d = {}
            L = int(d.get("L", 0))       # -100 ~ 100
            R = int(d.get("R", 0))
            ms = int(d.get("ms", 1500))  # 持续毫秒
            self.send_cmd({"action": "raw_motor", "L": L, "R": R, "ms": ms})
            log(f"[调试] 原始电机 L={L} R={R} {ms}ms")
            return 200, "application/json", json.dumps({"ok": True})

        # ---- 串口命令转发（BACKTEST / FWD / BACK / LEFT / RIGHT 等）----
        if path == "/api/serial" and body:
            try:
                d = json.loads(body)
            except Exception:
                d = {}
            line = (d.get("cmd") or "").strip().upper()
            if not line:
                return 400, "application/json", json.dumps({"error": "empty"})
            topic = getattr(cfg, "MQTT_TOPIC_CMD", "robot/cmd")
            self.client.publish(topic, json.dumps({"action": "serial",
                                                   "cmd": line}))
            log(f"[调试] 串口命令: {line}")
            return 200, "application/json", json.dumps({"ok": True})

        return 404, "application/json", json.dumps({"error": "not found"})


# ===========================================================================
# 简易 HTTP + WebSocket 服务器（给网页用）
# ===========================================================================
class WebServer(threading.Thread):
    def __init__(self, brain, port=8080):
        super().__init__(daemon=True)
        self.brain = brain
        self.port = port
        self.clients = []

    def run(self):
        import http.server
        import socketserver

        brain = self.brain
        web_root = HERE.parent / "web"

        class Handler(http.server.SimpleHTTPRequestHandler):
            # Python 3.13+ 移除了 SimpleHTTPRequestHandler 的 directory 参数，
            # 所以自己实现静态文件分发，不用父类的。
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)

            def log_message(self, *a):
                pass

            def _json(self, status, data):
                raw = json.dumps(data, ensure_ascii=False).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(raw)

            def _serve_static(self):
                """只提供 web/ 目录下的文件，防目录穿越"""
                path = self.path.split("?")[0].lstrip("/")
                if path == "":
                    path = "index.html"
                try:
                    safe = (web_root / path).resolve()
                    safe.relative_to(web_root.resolve())
                except Exception:
                    self._json(403, {"error": "forbidden"})
                    return
                if not safe.is_file():
                    self._json(404, {"error": "not found"})
                    return

                ctype = {
                    ".html": "text/html; charset=utf-8",
                    ".css": "text/css; charset=utf-8",
                    ".js": "application/javascript; charset=utf-8",
                    ".png": "image/png",
                    ".jpg": "image/jpeg",
                }.get(safe.suffix, "application/octet-stream")

                data = safe.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                if self.path.startswith("/api/"):
                    status, ctype, data = brain.handle_http(self.path, None)
                    self._json(status, data)
                    return
                self._serve_static()

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8") if length else ""
                status, ctype, data = brain.handle_http(self.path, body)
                self._json(status, data)

        class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
            daemon_threads = True
            allow_reuse_address = True

        try:
            srv = Server(("0.0.0.0", self.port), Handler)
        except OSError as e:
            # Windows 上 8080 等端口常被 Hyper-V/WSL 保留（WinError 10013），
            # 或者已被别的程序占用。自动往后找一个能用的。
            log(f"[网页] 端口 {self.port} 不可用（{e}），尝试其他端口")
            srv = None
            for cand in range(self.port + 1, self.port + 20):
                try:
                    srv = Server(("0.0.0.0", cand), Handler)
                    self.port = cand
                    log(f"[网页] 改用端口 {cand}")
                    break
                except OSError:
                    continue
            if srv is None:
                log("[网页] 找不到可用端口，网页遥控不可用")
                return

        try:
            import socket as _s
            host = _s.gethostbyname(_s.gethostname())
        except Exception:
            host = "127.0.0.1"
        log(f"[网页] 本机 http://127.0.0.1:{self.port}")
        log(f"[网页] 手机 http://{host}:{self.port}  (需同一 WiFi)")
        srv.serve_forever()


# ===========================================================================
def main():
    log("=" * 60)
    log(" 机器人 PC 端大脑")
    log("=" * 60)

    brain = RobotBrain()

    log(f"[AI] 后端={brain.ai.backend} 模型={brain.ai.model} "
        f"{'就绪' if brain.ai.ready() else '不可用'}")
    if not brain.ai.ready():
        log("[AI] 请在 config.py 里填 API Key")

    # MQTT：broker 没起来时不要直接崩，提示后继续跑（网页还能用）
    try:
        brain.client.connect(brain.mqtt_host, brain.mqtt_port, 60)
        brain.client.loop_start()
        log(f"[MQTT] 已连接 {brain.mqtt_host}:{brain.mqtt_port}")
    except Exception as e:
        log(f"[MQTT] 连接失败: {e}")
        log(f"[MQTT] 先启动 broker：mosquitto -c iot-broker/mosquitto.conf")
        log("[MQTT] 网页遥控仍可使用（只是收不到机器人状态）")
        brain.client.loop_start()   # 让 paho 自己重连

    # 网页
    web_port = getattr(cfg, "WEB_PORT", 8080)
    WebServer(brain, web_port).start()

    # 危险自动播报
    danger_threshold = getattr(cfg, "DANGER_DISTANCE", 20)
    last_alert = 0
    alert_cooldown = getattr(cfg, "ALERT_COOLDOWN", 30)

    try:
        while True:
            time.sleep(1)
            s = brain.state
            if not s:
                continue

            # 对话模式下不自动告警（避免打断用户说话）
            if s.get("chat") == 1:
                continue

            dist = s.get("dist", 999)
            pir = s.get("pir", 0)
            if dist <= danger_threshold and pir == 1:
                if time.time() - last_alert > alert_cooldown:
                    last_alert = time.time()
                    log(f"[告警] 距离 {dist}cm 检测到人")
                    brain.reply("有情况！检测到有人靠近", source="auto")
    except KeyboardInterrupt:
        log("[系统] 退出")
    finally:
        brain.client.loop_stop()
        brain.client.disconnect()


if __name__ == "__main__":
    main()
