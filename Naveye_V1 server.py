"""
PC Server with ACCURATE STT using Gemini AI
Optimized settings for best transcription accuracy
"""

import socket
import threading
import json
import os
import time
import struct
import glob
import shutil
import requests
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
import google.generativeai as genai
from gtts import gTTS
from http.server import HTTPServer, SimpleHTTPRequestHandler

# ========== GEMINI AI FOR STT ==========
try:
    import google.generativeai as genai
except ImportError:
    os.system("pip install google-generativeai")
    import google.generativeai as genai

# ========== CONFIGURATION ==========
GEMINI_API_KEY = 'Your_API_KEY'
genai.configure(api_key=GEMINI_API_KEY)
vision_model = genai.GenerativeModel('gemini-3.1-flash-lite')
stt_model    = genai.GenerativeModel('gemini-3.1-flash-lite')

STT_PROMPT = """You are an STT (Speech-to-Text) service. Your only task is to transcribe the audio I'm sending you.
Output ONLY the exact words spoken in the audio. Do not add any explanations, extra text, or formatting.
Just return the transcribed text as is. Do not include any markdown, quotes, or additional commentary."""

# Directories
AUDIO_DIR = "received_audio"
IMAGE_DIR = "received_images"
TTS_DIR   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tts_files")
FACES_DIR = "faces"
for d in [AUDIO_DIR, IMAGE_DIR, TTS_DIR, FACES_DIR]:
    os.makedirs(d, exist_ok=True)

# ── Auto-detect PC's LAN IP ──────────────────────────────────
def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

SERVER_IP = get_local_ip()
S3_PORT   = 8080
CAM_PORT  = 8081
TTS_PORT  = 8082

# SOS Email
SOS_GMAIL_SENDER   = "YOUR_EMAIL"
SOS_GMAIL_PASSWORD = "APP_PASS"
SOS_FAMILY_EMAILS  = ["YOUR_FAMILY_EMAIL"]
SOS_DEVICE_NAME    = "NAVE"
SOS_USER_NAME      = "YOUR_NAME"

# Global state
esp32S3_client  = None
esp32cam_client = None
client_lock     = threading.Lock()
custom_prompt   = """You are an assistive vision AI for a blind person name Nova. Analyze the given image and provide short, useful, and safety-focused output. Identify important objects such as people, vehicles, obstacles, chairs, walls, poles, stairs, doors, tables, bicycles, and animals. Identify useful readable text visible in the image. Focus only on important objects, navigation information, and useful text. Determine whether important objects are on the left, center, or right. Estimate whether important objects appear very close, close, or far. Do not provide exact measurements. Analyze the scene and determine the safest navigation guidance. Examples include: Go forward Move left Move right Path blocked, move left Path blocked, move right Stop Prioritize safety above all else. Hazards, obstacles, and moving objects are more important than text. Keep the response extremely short and simple. Mention only the most important objects and information. Avoid repetition. Do not explain your reasoning. Do not describe the entire scene. Output only the final guidance and important information. If no important objects or text are detected, output: Path clear. Go forward. Example outputs: Go forward Person ahead | close Exit on right Move right Obstacle ahead | very close Path blocked, move left Vehicle ahead | close Path clear. Go forward."""

last_image_path   = None
last_image_time   = 0
image_ready_event = threading.Event()
tts_done_event    = threading.Event()
continuous_mode   = False
continuous_thread = None

# ========== STT ==========
def transcribe_with_gemini(audio_path):
    try:
        print(f"[Gemini STT] Transcribing: {os.path.basename(audio_path)}")
        with open(audio_path, 'rb') as f:
            audio_bytes = f.read()
        if len(audio_bytes) < 1000:
            print("[Gemini STT] Audio too short")
            return None
        response = stt_model.generate_content([
            STT_PROMPT,
            {"mime_type": "audio/wav", "data": audio_bytes}
        ])
        transcript = response.text.strip()
        transcript = transcript.replace('```', '').replace('"', '').replace("'", "")
        if transcript:
            fillers = ["um", "uh", "like", "actually", "basically", "literally", "you know"]
            words = transcript.split()
            cleaned = [w for w in words if w.lower().strip('.,!?') not in fillers or len(words) == 1]
            transcript = ' '.join(cleaned)
            transcript = ' '.join(
                w.capitalize() if len(w) > 2 and w[0].isalpha() else w
                for w in transcript.split()
            )
        print(f"[Gemini STT] Transcript: '{transcript}'")
        if transcript and len(transcript) > 1 and transcript.lower() not in ['', 'the', 'a', 'an']:
            return transcript
        return None
    except Exception as e:
        print(f"[Gemini STT] Error: {e}")
        return None

def transcribe_with_fallback(audio_path):
    result = transcribe_with_gemini(audio_path)
    if result:
        return result
    print("[STT] Gemini failed, no fallback available")
    return None

# ========== TTS HTTP SERVER ==========
def start_tts_http():
    try:
        handler = lambda *a, **kw: SimpleHTTPRequestHandler(
            *a, directory=TTS_DIR, **kw
        )
        httpd = HTTPServer(('0.0.0.0', TTS_PORT), handler)
        print(f"[TTS] HTTP server on port {TTS_PORT} → serving {TTS_DIR}")
        httpd.serve_forever()
    except Exception as e:
        print(f"[TTS] HTTP server FAILED: {e}")

def generate_tts(text):
    fname = f"tts_{int(time.time()*1000)}.mp3"
    fpath = os.path.join(TTS_DIR, fname)
    gTTS(text=text, lang='en', slow=False).save(fpath)
    url = f"http://{SERVER_IP}:{TTS_PORT}/{fname}"
    print(f"[TTS] Generated: {url}")
    return url

# ========== NETWORK HELPERS ==========
def send_message(sock, data):
    if sock is None:
        return False
    if isinstance(data, dict):
        data = json.dumps(data).encode()
    elif isinstance(data, str):
        data = data.encode()
    try:
        sock.sendall(struct.pack('>I', len(data)) + data)
        return True
    except Exception as e:
        print(f"[NET] Send error: {e}")
        return False

# ========== GEMINI VISION ==========
def call_gemini_with_retry(parts, retries=3):
    for i in range(retries):
        try:
            response = vision_model.generate_content(parts)
            return response.text.strip()
        except Exception as e:
            err = str(e)
            print(f"[Gemini] Attempt {i+1} failed: {err}")
            if '429' in err or '500' in err or '503' in err:
                wait = 5 * (i + 1)
                print(f"[Gemini] Waiting {wait}s...")
                time.sleep(wait)
            else:
                break
    return "Sorry, I could not process that."

def process_audio_only(audio_path):
    print("[Gemini] Processing audio only...")
    with open(audio_path, 'rb') as f:
        audio_bytes = f.read()
    text = call_gemini_with_retry([
        "You are a helpful AI assistant name Nova. Don't repeat what I say. Don't repeat my question. When answering, don't use any symbols or markdown. Answer the question in 30-40 words. Copy the language of the person in the audio.",
        {"mime_type": "audio/wav", "data": audio_bytes}
    ])
    print(f"[Gemini] Response: {text[:200]}")
    send_text_to_esp32(text)

def process_audio_and_image(audio_path, image_path):
    print("[Gemini] Processing audio + image...")
    with open(audio_path, 'rb') as f:
        audio_bytes = f.read()
    with open(image_path, 'rb') as f:
        image_bytes = f.read()
    text = call_gemini_with_retry([
        "You are a helpful AI assistant name Nova. Don't repeat my question. Don't use any symbols or markdown. Answer the question in 40-50 words. Copy the language of the person in the audio.",
        {"mime_type": "audio/wav", "data": audio_bytes},
        {"mime_type": "image/jpeg", "data": image_bytes}
    ])
    print(f"[Gemini] Response: {text[:200]}")
    send_text_to_esp32(text)

def process_image_only(image_path):
    print("[Gemini] Processing image only...")
    with open(image_path, 'rb') as f:
        image_bytes = f.read()
    text = call_gemini_with_retry([
        custom_prompt,
        {"mime_type": "image/jpeg", "data": image_bytes}
    ])
    print(f"[Gemini] Response: {text[:200]}")
    return text

def send_text_to_esp32(text):
    deadline = time.time() + 10
    while not esp32S3_client and time.time() < deadline:
        time.sleep(0.5)
    if not esp32S3_client:
        print("[S3] No client")
        return
    try:
        tts_url = generate_tts(text)
        send_message(esp32S3_client, {
            "type":    "gemini_response",
            "text":    text,
            "tts_url": tts_url
        })
    except Exception as e:
        print(f"[TTS] Error: {e}")
        send_message(esp32S3_client, {
            "type":    "gemini_response",
            "text":    text,
            "tts_url": ""
        })

# ========== SOS ==========
def send_sos_email(device_id="Nova-ESP32S3Tiny", image_path=None):
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    subject = f"SOS ALERT — {SOS_USER_NAME} needs help!"
    body = f"""
EMERGENCY SOS ALERT from {SOS_DEVICE_NAME}

Time: {timestamp}
Device: {device_id}

Please contact {SOS_USER_NAME} immediately.
"""
    for recipient in SOS_FAMILY_EMAILS:
        if recipient and recipient != "@gmail.com":
            try:
                msg = MIMEMultipart()
                msg['From']    = SOS_GMAIL_SENDER
                msg['To']      = recipient
                msg['Subject'] = subject
                msg.attach(MIMEText(body, 'plain'))

                # Attach image if available
                if image_path and os.path.exists(image_path):
                    from email.mime.image import MIMEImage
                    with open(image_path, 'rb') as f:
                        img_data = f.read()
                    img = MIMEImage(img_data, name=os.path.basename(image_path))
                    img.add_header('Content-Disposition', 'attachment',
                                   filename=os.path.basename(image_path))
                    msg.attach(img)
                    print(f"[SOS] Image attached: {os.path.basename(image_path)}")
                else:
                    print("[SOS] No image to attach")

                with smtplib.SMTP_SSL('smtp.gmail.com', 465, timeout=15) as smtp:
                    smtp.login(SOS_GMAIL_SENDER, SOS_GMAIL_PASSWORD)
                    smtp.send_message(msg)
                print(f"[SOS] Sent to {recipient}")
            except Exception as e:
                print(f"[SOS] Failed to {recipient}: {e}")


def handle_sos(data):
    print(f"\n{'='*50}")
    print(f"[SOS] *** EMERGENCY from {data.get('device_id', 'Unknown')} ***")
    print(f"{'='*50}")

    # Send TTS immediately — don't wait for image
    threading.Thread(target=send_text_to_esp32,
                     args=("SOS sent. Help is on the way.",),
                     daemon=True).start()

    # Capture image — give CAM more time
    sos_image_path = None
    if esp32cam_client:
        print("[SOS] Capturing image...")
        image_ready_event.clear()
        ok = send_message(esp32cam_client, {"type": "capture_image"})
        if ok:
            got = image_ready_event.wait(timeout=20)
            if got and last_image_path and os.path.exists(last_image_path):
                sos_image_path = last_image_path
                print(f"[SOS] Image captured: {os.path.basename(sos_image_path)}")
            else:
                print("[SOS] Image capture timed out")
        else:
            print("[SOS] Failed to send capture command")
    else:
        print("[SOS] CAM not connected")

    send_sos_email(data.get('device_id', 'Nova-ESP32S3Tiny'), sos_image_path)

# ========== FACE RECOGNITION ==========
def teach_face(audio_path):
    name = transcribe_with_gemini(audio_path)
    if not name:
        send_text_to_esp32("Sorry, I could not understand the name. Please try again clearly.")
        return
    name = name.strip().split()[0] if name.split() else name
    if not esp32cam_client:
        send_text_to_esp32("Camera not connected.")
        return
    print(f"[Face] Capturing for {name}...")
    image_ready_event.clear()
    send_message(esp32cam_client, {"type": "capture_image"})
    got = image_ready_event.wait(timeout=15)
    if not got or not last_image_path:
        send_text_to_esp32("Could not capture image.")
        return
    face_path = os.path.join(FACES_DIR, f"{name}.jpg")
    shutil.copy(last_image_path, face_path)
    print(f"[Face] Saved: {face_path}")
    send_text_to_esp32(f"Face learning done. {name} has been registered.")

def recognize_face():
    if not esp32cam_client:
        send_text_to_esp32("Camera not connected.")
        return
    image_ready_event.clear()
    send_message(esp32cam_client, {"type": "capture_image"})
    got = image_ready_event.wait(timeout=15)
    if not got or not last_image_path:
        send_text_to_esp32("Could not capture image.")
        return
    face_files = glob.glob(os.path.join(FACES_DIR, "*.jpg"))
    if not face_files:
        send_text_to_esp32("No faces registered.")
        return
    parts = ["Compare these faces. Reply NAME:PERCENTAGE or UNKNOWN:0"]
    for face_file in face_files:
        name = os.path.splitext(os.path.basename(face_file))[0]
        with open(face_file, 'rb') as f:
            parts.append({"mime_type": "image/jpeg", "data": f.read()})
        parts.append(f"Registered: {name}")
    with open(last_image_path, 'rb') as f:
        parts.append({"mime_type": "image/jpeg", "data": f.read()})
    parts.append("Identify this person")
    try:
        response = vision_model.generate_content(parts)
        result = response.text.strip()
        if "UNKNOWN" in result.upper():
            send_text_to_esp32("Unknown person detected.")
        else:
            send_text_to_esp32(result)
    except Exception as e:
        print(f"[Face] Error: {e}")
        send_text_to_esp32("Face recognition failed.")

# ========== CONTINUOUS MODE ==========
def continuous_loop():
    global continuous_mode
    print("[Continuous] Started")
    while continuous_mode:
        if not esp32S3_client:
            print("[Continuous] ESP32 not connected, stopping")
            continuous_mode = False
            break

        cam_wait = 0
        while not esp32cam_client and continuous_mode:
            if cam_wait == 0:
                print("[Continuous] Waiting for camera to reconnect...")
            cam_wait += 1
            time.sleep(0.1)
            if cam_wait > 100:
                break
        if not continuous_mode:
            break
        if not esp32cam_client:
            time.sleep(1)
            continue

        image_ready_event.clear()
        ok = send_message(esp32cam_client, {"type": "capture_image"})
        if not ok:
            print("[Continuous] Camera send failed, waiting for reconnect...")
            time.sleep(1)
            continue

        if not image_ready_event.wait(timeout=20):
            print("[Continuous] No image received (timeout), retrying...")
            time.sleep(1)
            continue

        if not last_image_path:
            time.sleep(1)
            continue

        text = process_image_only(last_image_path)
        if not text or not continuous_mode:
            break
        try:
            tts_url = generate_tts(text)
            tts_done_event.clear()
            send_message(esp32S3_client, {
                "type":    "gemini_response",
                "text":    text,
                "tts_url": tts_url
            })
            print("[Continuous] Waiting for TTS done...")
            tts_done_event.wait(timeout=30)
        except Exception as e:
            print(f"[Continuous] Error: {e}")
            time.sleep(2)
    print("[Continuous] Stopped")

def handle_button_press_double():
    global continuous_mode, continuous_thread
    if not continuous_mode:
        continuous_mode = True
        print("[Mode] Continuous: ON")
        continuous_thread = threading.Thread(target=continuous_loop, daemon=True)
        continuous_thread.start()
    else:
        continuous_mode = False
        print("[Mode] Continuous: OFF")

# ========== ESP32 HANDLERS ==========
def handle_esp32S3(conn, addr):
    global esp32S3_client, continuous_mode
    with client_lock:
        esp32S3_client = conn
    print(f"[+] S3 connected: {addr[0]}")

    try:
        while True:
            hdr = conn.recv(4)
            if not hdr:
                break
            msg_len = struct.unpack('>I', hdr)[0]
            body = b''
            while len(body) < msg_len:
                chunk = conn.recv(msg_len - len(body))
                if not chunk:
                    break
                body += chunk

            data     = json.loads(body.decode('utf-8'))
            msg_type = data.get('type')

            if msg_type == 'tts_done':
                tts_done_event.set()

            elif msg_type == 'sos':
                threading.Thread(target=handle_sos, args=(data,), daemon=True).start()

            elif msg_type == 'button_press':
                if data.get('press_type') == 'double':
                    handle_button_press_double()

            elif msg_type == 'recognize_face':
                threading.Thread(target=recognize_face, daemon=True).start()

            elif msg_type == 'battery_status':
                pct = data.get('percent', 0)
                if pct < 25:
                    text = f"Warning. Battery is critically low at {pct} percent. Please charge your Nova assistant."
                elif pct < 50:
                    text = f"Nova is online. Battery is low at {pct} percent."
                else:
                    text = f"Nova is online. Battery is at {pct} percent."
                tts_url = generate_tts(text)
                send_message(conn, {
                    "type":    "gemini_response",
                    "text":    text,
                    "tts_url": tts_url
                })

            elif msg_type == 'tts_speak':
                text    = data.get('text', '')
                tts_url = generate_tts(text)
                send_message(conn, {"type": "tts_url", "url": tts_url})

            elif msg_type == 'audio_start':
                size     = data.get('size', 0)
                raw      = bytearray(size)
                received = 0
                conn.settimeout(120)
                while received < size:
                    n = conn.recv_into(memoryview(raw)[received:], size - received)
                    if not n:
                        break
                    received += n
                conn.settimeout(None)
                path = os.path.join(AUDIO_DIR, f"audio_{int(time.time())}.wav")
                with open(path, 'wb') as f:
                    f.write(raw)
                threading.Thread(target=process_audio_job, args=(path,), daemon=True).start()

            elif msg_type == 'teach_face':
                size     = data.get('size', 0)
                raw      = bytearray(size)
                received = 0
                conn.settimeout(120)
                while received < size:
                    n = conn.recv_into(memoryview(raw)[received:], size - received)
                    if not n:
                        break
                    received += n
                conn.settimeout(None)
                path = os.path.join(AUDIO_DIR, f"teach_{int(time.time())}.wav")
                with open(path, 'wb') as f:
                    f.write(raw)
                threading.Thread(target=teach_face, args=(path,), daemon=True).start()

    except Exception as e:
        print(f"[S3] Error: {e}")
    finally:
        continuous_mode = False
        with client_lock:
            esp32S3_client = None
        conn.close()
        print("[-] S3 disconnected")

def handle_esp32cam(conn, addr):
    global esp32cam_client, last_image_path
    with client_lock:
        esp32cam_client = conn
    print(f"[+] CAM connected: {addr[0]}")
    conn.settimeout(None)
    try:
        while True:
            hdr = conn.recv(4)
            if not hdr or len(hdr) < 4:
                print(f"[CAM] Bad header: {hdr}")
                break
            msg_len = struct.unpack('>I', hdr)[0]
            print(f"[CAM] msg_len={msg_len}")

            if msg_len == 0 or msg_len > 8192:
                print(f"[CAM] Filtered out msg_len={msg_len}")
                while client.available(): conn.recv(1)
                continue

            body = b''
            while len(body) < msg_len:
                chunk = conn.recv(msg_len - len(body))
                if not chunk:
                    print("[CAM] Body read failed")
                    break
                body += chunk

            print(f"[CAM] body={body}")

            try:
                data = json.loads(body.decode('utf-8'))
            except Exception as e:
                print(f"[CAM] JSON parse error: {e}")
                continue

            msg_type = data.get('type')
            print(f"[CAM] type={msg_type}")

            if data.get('type') == 'ping':
                continue

            if data.get('type') == 'image_start':
                size = data.get('size', 0)
                print(f"[CAM] image_start size={size}")
                if size <= 0 or size > 500000:
                    print(f"[CAM] Invalid size: {size}")
                    continue

                try:
                    print("[CAM] Sending ACK...")
                    conn.send(struct.pack('>I', 1))
                    print("[CAM] ACK sent")
                except Exception as e:
                    print(f"[CAM] ACK send failed: {e}")
                    continue

                raw = bytearray(size)
                received = 0
                conn.settimeout(25)
                try:
                    while received < size:
                        n = conn.recv_into(memoryview(raw)[received:], size - received)
                        if not n:
                            break
                        received += n
                finally:
                    conn.settimeout(None)

                if received < size:
                    print(f"[CAM] Incomplete image: {received}/{size}")
                    continue

                path = os.path.join(IMAGE_DIR, f"image_{int(time.time_ns())}.jpg")
                with open(path, 'wb') as f:
                    f.write(raw)
                last_image_path = path
                image_ready_event.set()
                print(f"[CAM] Image saved: {os.path.basename(path)}")

    except Exception as e:
        print(f"[CAM] Error: {e}")
    finally:
        with client_lock:
            esp32cam_client = None
        conn.close()
        print("[-] CAM disconnected")

def process_audio_job(audio_path):
    global last_image_path
    if esp32cam_client:
        image_ready_event.clear()
        send_message(esp32cam_client, {"type": "capture_image"})
        if image_ready_event.wait(timeout=20) and last_image_path:
            process_audio_and_image(audio_path, last_image_path)
            return
    process_audio_only(audio_path)

# ========== MAIN ==========
def accept_loop(sock, handler):
    while True:
        conn, addr = sock.accept()
        threading.Thread(target=handler, args=(conn, addr), daemon=True).start()

def main():
    print("\n" + "="*50)
    print("NOVA AI SERVER")
    print("="*50)
    print(f"  Server IP : {SERVER_IP}  <- ESP32 must reach this")
    print(f"  S3  Port  : {S3_PORT}")
    print(f"  CAM Port  : {CAM_PORT}")
    print(f"  TTS Port  : {TTS_PORT}")
    print(f"  TTS Dir   : {TTS_DIR}")
    print("="*50 + "\n")

    threading.Thread(target=start_tts_http, daemon=True).start()
    time.sleep(0.5)

    s_S3 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s_S3.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s_S3.bind(('0.0.0.0', S3_PORT))
    s_S3.listen(5)

    s_cam = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s_cam.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s_cam.bind(('0.0.0.0', CAM_PORT))
    s_cam.listen(5)

    threading.Thread(target=lambda: accept_loop(s_S3,  handle_esp32S3),  daemon=True).start()
    threading.Thread(target=lambda: accept_loop(s_cam, handle_esp32cam), daemon=True).start()

    print("Server running. Commands: status | stop | test_sos")
    while True:
        try:
            cmd = input("> ").strip().lower()
            if cmd == "status":
                print(f"  S3:         {'connected' if esp32S3_client  else 'disconnected'}")
                print(f"  CAM:        {'connected' if esp32cam_client else 'disconnected'}")
                print(f"  Continuous: {'ON' if continuous_mode else 'OFF'}")
                print(f"  Faces:      {len(glob.glob(os.path.join(FACES_DIR, '*.jpg')))}")
                print(f"  Server IP:  {SERVER_IP}")
            elif cmd == "stop":
                continuous_mode = False
            elif cmd == "test_sos":
                send_sos_email("TEST")
        except KeyboardInterrupt:
            break

if __name__ == "__main__":
    main()
