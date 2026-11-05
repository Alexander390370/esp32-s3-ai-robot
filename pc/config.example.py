# -*- coding: utf-8 -*-
"""
PC 端配置。复制成 config.py 后填写。
config.py 已被 .gitignore 排除，不会被提交。
"""

# ===========================================================================
# AI 后端：dashscope / deepseek / ark / zhipu / stepfun / openai / ollama
# ===========================================================================
# dashscope（通义千问）：延迟最低，0.5-1 秒，有免费额度 —— 推荐
AI_BACKEND = "deepseek"
AI_MODEL   = ""          # 留空用后端默认模型
AI_TIMEOUT = 30

# --- 阿里云百炼（通义） ---
# https://bailian.console.aliyun.com
DASHSCOPE_API_KEY = ""
DASHSCOPE_BASE_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1"

# --- DeepSeek（便宜，中文好） ---
# https://platform.deepseek.com
DEEPSEEK_API_KEY = ""
DEEPSEEK_BASE_URL = "https://api.deepseek.com"

# --- 火山方舟（豆包） ---
ARK_API_KEY = ""
ARK_BASE_URL = "https://ark.cn-beijing.volces.com/api/v3"

# --- 智谱 GLM ---
ZHIPU_API_KEY = ""
ZHIPU_BASE_URL = "https://open.bigmodel.cn/api/paas/v4"

# --- 阶跃星辰 ---
STEPFUN_API_KEY = ""
STEPFUN_BASE_URL = "https://api.stepfun.com/v1"

# --- OpenAI 兼容 ---
OPENAI_API_KEY  = ""
OPENAI_BASE_URL = "https://api.openai.com/v1"

# --- 本地 Ollama（不需要联网） ---
OLLAMA_BASE_URL = "http://127.0.0.1:11434/v1"
OLLAMA_API_KEY  = ""

# ===========================================================================
# MQTT
# ===========================================================================
# broker 跑在 PC 上，用 127.0.0.1 就行
# 换网络（PC IP 变了）时，固件里的 MQTT_HOST 也要改
MQTT_HOST = "127.0.0.1"
MQTT_PORT = 1884

MQTT_TOPIC_CMD    = "robot/cmd"      # PC → 机器人
MQTT_TOPIC_STATE  = "robot/state"    # 机器人 → PC
MQTT_TOPIC_AUDIO  = "robot/audio"    # 机器人 → PC（录音）
MQTT_TOPIC_SPEAK  = "robot/speak"    # PC → 机器人（音频）

# ===========================================================================
# 语音
# ===========================================================================
TTS_ENABLED = True

# 引擎：edge = 微软在线神经网络语音（音质接近真人，需联网，免费）
#       sapi = Windows 本地（离线可用，但声音机械）
TTS_ENGINE = "edge"

# 可选的 edge 中文语音：
#   zh-CN-XiaoxiaoNeural   女声，温柔自然（默认，推荐）
#   zh-CN-XiaoyiNeural     女声，活泼
#   zh-CN-YunxiNeural      男声，阳光
#   zh-CN-YunjianNeural    男声，浑厚
#   zh-CN-liaoning-XiaobeiNeural  东北话
#   zh-CN-shaanxi-XiaoniNeural    陕西话
TTS_VOICE = "zh-CN-XiaoxiaoNeural"

# edge 参数：语速和音调
#   rate:  "+0%" 正常，"+10%" 快一点，"-10%" 慢一点
#   pitch: "+0Hz" 正常，"+5Hz" 音调高一点
TTS_RATE  = "+8%"
TTS_PITCH = "+2Hz"

# 合成采样率：必须和固件 config.h 的 I2S_SAMPLE_RATE 一致
TTS_SAMPLE_RATE = 16000

# 语音识别（可选）。装：pip install openai-whisper
# 模型：tiny / base / small（越准越慢）
ASR_MODEL = "small"
ASR_SAMPLE_RATE = 16000

# ===========================================================================
# 行为
# ===========================================================================
DANGER_DISTANCE = 20      # 危险距离（与固件保持一致）
ALERT_COOLDOWN  = 30      # 自动告报的冷却（秒）
WEB_PORT        = 8765   # 网页遥控端口
