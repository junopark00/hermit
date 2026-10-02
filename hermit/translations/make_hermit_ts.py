"""Builds app/languages/hermit_ko.ts (and .qm) for the strings Hermit added.

BrandingTranslator consults hermit_<lang> first, then the language's qml_<lang>.ts. Add new strings to TRANSLATIONS below, keyed by (context, source text) exactly as in the
code: the context is the QML file name or the C++ class (or the first argument of
QCoreApplication::translate). Then run:

    python hermit/translations/make_hermit_ts.py

lrelease is the LRELEASE environment variable if set, else bin/lrelease.exe in the Qt kit folder
HERMIT_QT_DIR, else C:/Qt/6.11.3/msvc2022_64/bin/lrelease.exe.
"""
import os
import subprocess
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
TS = os.path.join(REPO, 'app', 'languages', 'hermit_ko.ts')
QT_DIR = os.environ.get('HERMIT_QT_DIR', r'C:\Qt\6.11.3\msvc2022_64')
LRELEASE = os.environ.get('LRELEASE', os.path.join(QT_DIR, 'bin', 'lrelease.exe'))

# (context, source) -> Korean. A tuple value marks a plural (%n) message.
TRANSLATIONS = {
    ('ClipboardSync', 'Image sent to the host (%1)'): '이미지를 호스트로 보냈습니다 (%1)',
    ('ClipboardSync', '%n item(s) sent to the host (%1)'): ('%n개 항목을 호스트로 보냈습니다 (%1)',),
    ('ClipboardSync', 'Image from the host is ready to paste'): '호스트의 이미지를 붙여넣을 수 있습니다',
    ('ClipboardSync', '%n item(s) from the host are ready to paste'): ('호스트의 %n개 항목을 붙여넣을 수 있습니다',),


    ('ReconnectDialog', 'The host is offline.'): '호스트가 오프라인입니다.',
    ('ReconnectDialog', 'Connection lost'): '연결 끊김',
    ('ReconnectDialog', 'The connection was lost. Reconnecting in %1 s.'): '연결이 끊겼습니다. %1초 후 다시 연결합니다.',
    ('ReconnectDialog', 'The host is offline. Checking again in %1 s.'): '호스트가 오프라인입니다. %1초 후 다시 확인합니다.',
    ('ReconnectDialog', 'Attempt %1 of %2'): '시도 %1/%2',
    ('ReconnectDialog', 'Cancel'): '취소',
    ('ReconnectDialog', 'Connect now'): '지금 연결',
    ('SettingsView', 'Reconnect automatically when the connection drops'): '연결이 끊기면 자동으로 다시 연결',
    ('SettingsView', 'If the network drops during a stream, connects to the same app again after 5 seconds, up to 3 times in a row. Not used when you end the stream yourself or the app exits on the host.'):
        '스트리밍 중 네트워크가 끊기면 5초 후 같은 앱에 다시 연결합니다(연속 3회까지). 직접 스트림을 끝냈거나 호스트에서 앱이 종료된 경우에는 동작하지 않습니다.',
    ('Session', 'A different app is now running on %1.'): '%1에서 다른 앱이 실행 중입니다.',

    ('ClipboardSync', 'Sending to the host: %1% (%2)'): '호스트로 보내는 중 %1% (%2)',
    ('ClipboardSync', 'Receiving from the host: %1% (%2)'): '호스트에서 받는 중 %1% (%2)',
    ('ClipboardSync', 'Receiving from the host (%1)'): '호스트에서 받는 중 (%1)',
    ('ClipboardSync', 'Ctrl+Alt+Shift+T to cancel'): '취소: Ctrl+Alt+Shift+T',
    ('ClipboardSync', 'File transfer cancelled'): '파일 전송을 취소했습니다',
    ('ClipboardSync', 'File transfer failed'): '파일 전송에 실패했습니다',
    ('SettingsView', 'Image and file transfer speed limit'): '이미지·파일 전송 속도 제한',
    ('SettingsView', 'Unlimited'): '제한 없음',
    ('SettingsView', 'Caps how fast clipboard images and files move in either direction, so a large transfer does not take bandwidth from the stream. Press Ctrl+Alt+Shift+T while streaming to cancel a transfer.'):
        '클립보드 이미지와 파일을 주고받는 속도의 상한입니다. 큰 전송이 스트림의 대역폭을 빼앗지 않게 합니다. 스트리밍 중 Ctrl+Alt+Shift+T를 누르면 전송을 취소합니다.',

    ('SettingsView', 'Performance Overlay'): '성능 오버레이',
    ('SettingsView', 'Metrics to show'): '표시할 지표',
    ('SettingsView', 'Video (resolution, FPS, codec)'): '비디오 (해상도, FPS, 코덱)',
    ('SettingsView', 'Bitrate'): '비트레이트',
    ('SettingsView', 'Network loss'): '네트워크 손실',
    ('SettingsView', 'Round trip time'): '왕복 지연',
    ('SettingsView', 'Host latency'): '호스트 지연',
    ('SettingsView', 'Estimated total latency'): '추정 전체 지연',
    ('SettingsView', 'Received / decoded / rendered FPS'): '수신 / 디코드 / 렌더 FPS',
    ('SettingsView', 'Jitter drops'): '지터 폐기',
    ('SettingsView', 'Decode time'): '디코드 시간',
    ('SettingsView', 'Queue delay'): '큐 대기',
    ('SettingsView', 'Render time'): '렌더 시간',
    ('SettingsView', 'Decoder'): '디코더',
    ('SettingsView', 'Text size'): '글자 크기',
    ('SettingsView', 'Small'): '작게',
    ('SettingsView', 'Medium'): '보통',
    ('SettingsView', 'Large'): '크게',
    ('SettingsView', 'Restore defaults'): '기본값으로',

    ('StatsOverlay', 'Video'): '비디오',
    ('StatsOverlay', 'Received / decoded / rendered'): '수신 / 디코드 / 렌더 FPS',
    ('StatsOverlay', 'Bitrate'): '비트레이트',
    ('StatsOverlay', '%1 Mbps (peak %2)'): '%1 Mbps (최대 %2)',
    ('StatsOverlay', 'Network loss'): '네트워크 손실',
    ('StatsOverlay', 'Jitter drops'): '지터 폐기',
    ('StatsOverlay', 'Round trip'): '왕복 지연',
    ('StatsOverlay', 'Host latency'): '호스트 지연',
    ('StatsOverlay', 'Decode'): '디코드',
    ('StatsOverlay', 'Queue delay'): '큐 대기',
    ('StatsOverlay', 'Render'): '렌더',
    ('StatsOverlay', 'Estimated total latency'): '추정 전체 지연',
    ('StatsOverlay', 'Decoder'): '디코더',

    ('ConnectionProfiles', 'full screen'): '전체 화면',
    ('ConnectionProfiles', 'borderless'): '테두리 없는 창',
    ('ConnectionProfiles', 'window'): '창 모드',
    ('ConnectionProfiles', 'frame pacing'): '프레임 조율',
    ('ConnectionProfiles', 'V-Sync'): '수직동기화',

    ('ProfilesButton', 'Profile: %1'): '프로필: %1',
    ('ProfilesButton', 'Connection profiles'): '연결 프로필',
    ('ProfilesButton', 'Applied %1: %2'): '%1 적용: %2',
    ('ProfilesButton', 'No profiles yet'): '저장된 프로필이 없습니다',
    ('ProfilesButton', 'Save current settings as profile...'): '현재 설정을 프로필로 저장...',
    ('ProfilesButton', 'Delete profile'): '프로필 삭제',
    ('ProfilesButton', 'Save connection profile'): '연결 프로필 저장',
    ('ProfilesButton', 'Saved %1: %2'): '%1 저장: %2',
    ('ProfilesButton', 'Saves resolution, frame rate, bitrate, display mode, V-Sync, frame pacing, codec, HDR, YUV 4:4:4, audio, the performance overlay and large packets. A profile with the same name is replaced.'):
        '해상도, 프레임 레이트, 비트레이트, 창 모드, 수직동기화, 프레임 조율, 코덱, HDR, YUV 4:4:4, 오디오, 성능 오버레이, 큰 패킷 설정을 저장합니다. 같은 이름의 프로필은 덮어씁니다.',
    ("ProfilesButton", "Delete the profile '%1'?"): "'%1' 프로필을 삭제할까요?",
    ('ProfilesButton', 'For example: 1440p windowed'): '예: 1440p 창 모드',

    ('SessionSummaryDialog', 'Frame pacing added %1 ms of queue delay. If the picture stays smooth, turn it off in Settings or in the stream settings (Ctrl+Alt+Shift+P) for faster response.'):
        '프레임 조율 때문에 큐 대기가 %1 ms 늘었습니다. 화면이 계속 부드럽다면 설정이나 스트림 설정(Ctrl+Alt+Shift+P)에서 끄면 반응이 빨라집니다.',
    ('SessionSummaryDialog', 'Automatic · average %1 Mbps (up to %2)'): '자동 · 평균 %1 Mbps (최대 %2)',
    ('SessionSummaryDialog', 'Average %1 Mbps (last %2)'): '평균 %1 Mbps (마지막 %2)',
    ('SessionSummaryDialog', 'Many frames were lost on the network. Try a lower bitrate or a wired connection.'):
        '네트워크에서 잃은 프레임이 많습니다. 비트레이트를 낮추거나 유선 연결을 써 보세요.',
    ('SessionSummaryDialog', 'The host sends frames only when the screen changes, so the average FPS is lower on quiet screens. Judge smoothness by network loss and jitter drops.'):
        '호스트는 화면이 바뀔 때만 프레임을 보내므로 화면 변화가 적으면 평균 FPS가 낮게 나옵니다. 끊김은 네트워크 손실과 지터 폐기로 판단하세요.',
    ('SessionSummaryDialog', '%1 min'): '%1분',
    ('SessionSummaryDialog', '%1 h %2 min'): '%1시간 %2분',
    ('SessionSummaryDialog', 'Session summary'): '세션 요약',
    ('SessionSummaryDialog', 'Good'): '양호',
    ('SessionSummaryDialog', 'Check'): '주의',
    ('SessionSummaryDialog', 'Poor'): '나쁨',
    ('SessionSummaryDialog', 'Jitter drops'): '지터 폐기',
    ('SessionSummaryDialog', 'Host latency, average'): '호스트 지연 (평균)',
    ('SessionSummaryDialog', 'Host latency, maximum'): '호스트 지연 (최대)',
    ('SessionSummaryDialog', 'Round trip (RTT)'): '왕복 지연 (RTT)',
    ('SessionSummaryDialog', 'Host'): '호스트',
    ('SessionSummaryDialog', 'App'): '앱',
    ('SessionSummaryDialog', 'Duration'): '시간',
    ('SessionSummaryDialog', 'Ended'): '종료',
    ('SessionSummaryDialog', 'Video'): '비디오',
    ('SessionSummaryDialog', 'Bitrate'): '비트레이트',
    ('SessionSummaryDialog', 'Display'): '표시',
    ('SessionSummaryDialog', 'Frame pacing'): '프레임 조율',
    ('SessionSummaryDialog', 'V-Sync'): '수직동기화',
    ('SessionSummaryDialog', 'Metric'): '지표',
    ('SessionSummaryDialog', 'This session'): '이번 세션',
    ('SessionSummaryDialog', 'Previous 10 avg.'): '지난 10회 평균',
    ('SessionSummaryDialog', 'Status'): '상태',
    ('SessionSummaryDialog', 'Average FPS'): '평균 FPS',
    ('SessionSummaryDialog', 'Network loss'): '네트워크 손실',
    ('SessionSummaryDialog', 'Queue delay'): '큐 대기',
    ('SessionSummaryDialog', 'Decode / render'): '디코드 / 렌더',
    ('SessionSummaryDialog', 'Window'): '창 모드',
    ('SessionSummaryDialog', 'Full screen'): '전체 화면',
    ('SessionSummaryDialog', 'Open history'): '기록 열기',
    ('SessionSummaryDialog', "Don't show again"): '다시 보지 않기',
    ('SessionSummaryDialog', 'Close'): '닫기',

    ('SettingsView', 'Sync clipboard with the host'): '호스트와 클립보드 동기화',
    ('SettingsView', 'Renderer'): '렌더러',
    ('StreamPanel', 'Stream settings'): '스트림 설정',
    ('StreamPanel', 'Back to the stream (Esc)'): '스트림으로 돌아가기 (Esc)',
    ('StreamPanel', 'BITRATE'): '비트레이트',
    ('StreamPanel', 'RESOLUTION, FRAME RATE AND CODEC (RECONNECT TO APPLY)'): '해상도·프레임·코덱 (다시 연결해 적용)',
    ('StreamPanel', 'The picture pauses for 2 to 3 seconds and the host app keeps running.'):
        '화면이 2~3초 멈추고, 호스트의 앱은 계속 실행됩니다.',
    ('StreamPanel', 'Resolution'): '해상도',
    ('StreamPanel', 'Type any size as width x height, for example 720x1280 for a portrait screen.'):
        '가로x세로로 원하는 크기를 입력할 수 있습니다. 예: 세로 화면은 720x1280',
    ('StreamPanel', 'Frame rate'): '프레임 레이트',
    ('StreamPanel', 'Bitrate'): '비트레이트',
    ('StreamPanel', 'Recommended: %1 Mbps (HEVC/AV1: %2 Mbps)'): '권장: %1 Mbps (HEVC/AV1은 %2 Mbps)',
    ('StreamPanel', 'Recommended: %1 Mbps'): '권장: %1 Mbps',
    ('StreamPanel', 'Video codec'): '비디오 코덱',
    ('StreamPanel', 'Automatic'): '자동 (권장)',
    ('StreamPanel', 'HDR streaming is not supported on this PC.'): '이 PC에서는 HDR 스트리밍을 지원하지 않습니다.',
    ('StreamPanel', 'Apply and reconnect'): '적용하고 다시 연결',
    ('StreamPanel', 'Revert'): '되돌리기',
    ('StreamPanel', 'DISPLAY'): '표시',
    ('StreamPanel', 'Applied at once.'): '바로 적용됩니다.',
    ('StreamPanel', 'Performance overlay'): '성능 오버레이',
    ('StreamPanel', 'Video'): '비디오',
    ('StreamPanel', 'Network loss'): '네트워크 손실',
    ('StreamPanel', 'Round trip'): '왕복 지연',
    ('StreamPanel', 'Host latency'): '호스트 지연',
    ('StreamPanel', 'Total latency'): '추정 전체 지연',
    ('StreamPanel', 'Frame rates'): '단계별 FPS',
    ('StreamPanel', 'Jitter drops'): '지터 폐기',
    ('StreamPanel', 'Decode time'): '디코드 시간',
    ('StreamPanel', 'Queue delay'): '큐 대기',
    ('StreamPanel', 'Render time'): '렌더 시간',
    ('StreamPanel', 'Decoder'): '디코더',
    ('StreamPanel', 'Text size'): '글자 크기',
    ('StreamPanel', 'Small'): '작게',
    ('StreamPanel', 'Medium'): '보통',
    ('StreamPanel', 'Large'): '크게',
    ('StreamPanel', 'V-Sync'): '수직동기화',
    ('StreamPanel', 'Frame pacing'): '프레임 조율',
    ('StreamPanel', 'Changing V-Sync or frame pacing recreates the renderer: the picture blinks once.'):
        '수직동기화나 프레임 조율을 바꾸면 렌더러를 다시 만들어 화면이 한 번 깜빡입니다.',
    ('StreamPanel', 'INPUT AND SOUND'): '입력과 소리',
    ('StreamPanel', 'Mute on this PC'): '이 PC에서 소리 끄기',
    ('StreamPanel', 'Sync clipboard with the host'): '호스트와 클립보드 동기화',
    ('StreamPanel', 'Toggle full screen'): '전체 화면 전환',
    ('StreamPanel', 'Disconnect'): '연결 끊기',
    ('StreamPanel', 'Quit app and disconnect'): '앱 종료 후 연결 끊기',
    ('StreamPanel', 'Ctrl+Alt+Shift+P opens and closes this panel.'): 'Ctrl+Alt+Shift+P로 이 패널을 열고 닫습니다.',
    ('StreamPanel', 'Applying the bitrate...'): '비트레이트를 적용하는 중...',
    ('StreamPanel', 'The last bitrate change could not be confirmed; trying again every 5 seconds. Apply and reconnect if it keeps failing.'):
        '마지막 비트레이트 변경을 확인하지 못해 5초마다 다시 시도합니다. 계속 실패하면 다시 연결해 적용하세요.',
    ('Session', 'This host cannot change the bitrate during a stream; open the stream settings (Ctrl+Alt+Shift+P) to reconnect.'):
        '이 호스트는 스트리밍 중에 비트레이트를 바꿀 수 없습니다. 스트림 설정(Ctrl+Alt+Shift+P)에서 다시 연결하세요.',
    ('Session', 'The bitrate change could not be confirmed; trying again. See the stream settings (Ctrl+Alt+Shift+P).'):
        '비트레이트 변경을 확인하지 못해 다시 시도합니다. 스트림 설정(Ctrl+Alt+Shift+P)에서 확인하세요.',
    ('StreamPanel', 'This host cannot change the bitrate during a stream; it is applied by reconnecting.'):
        '이 호스트는 스트리밍 중에 비트레이트를 바꿀 수 없어 다시 연결해 적용합니다.',
    ('StreamPanel', 'Adjust automatically to the network'): '비트레이트 자동 조절',
    ('StreamPanel', 'Lowers the bitrate when frames are lost or the round trip rises, and raises it again up to the chosen bitrate when the network is calm. Needs a Shell host.'):
        '프레임 손실이나 왕복 지연이 늘면 비트레이트를 낮추고, 네트워크가 안정되면 설정한 비트레이트까지 다시 올립니다. Shell 호스트가 필요합니다.',
    ('StreamPanel', 'Automatic: now %1 Mbps (up to %2 Mbps)'): '자동 조절: 지금 %1 Mbps (최대 %2 Mbps)',
    ('SettingsView', 'Adjust the bitrate automatically to the network (Shell host)'): '비트레이트 자동 조절 (Shell 호스트)',
    ('SettingsView', 'While streaming, lowers the bitrate when frames are lost or the round trip rises, and raises it again up to the bitrate above when the network is calm, without reconnecting.'):
        '스트리밍 중 프레임 손실이나 왕복 지연이 늘면 비트레이트를 낮추고, 네트워크가 안정되면 위에서 정한 비트레이트까지 다시 올립니다. 다시 연결하지 않습니다.',

    # Files dropped on the stream window
    ('ClipboardSync', '%n item(s) copied to the host clipboard (%1). Paste with Ctrl+V on the host.'):
        ('%n개 항목을 호스트 클립보드에 넣었습니다 (%1). 호스트에서 Ctrl+V로 붙여넣으세요.',),
    ('ClipboardSync', 'Sending files needs a Shell host that allows clipboard and file transfer for this device'):
        '파일을 보내려면 이 기기에 클립보드·파일 전송을 허용한 Shell 호스트가 필요합니다',
    ('ClipboardSync', 'Files not sent: over %1 MB or %2 items, or a file cannot be read'):
        '파일을 보내지 못했습니다: %1MB 또는 %2개 항목을 넘었거나 읽을 수 없는 파일이 있습니다',
    ('ClipboardSync', 'Files not sent: the host does not allow file upload for this device'):
        '파일을 보내지 못했습니다: 호스트가 이 기기의 파일 업로드를 허용하지 않습니다',
    ('ClipboardSync', 'Files on the host not copied: the host does not allow file download for this device'):
        '호스트의 파일을 가져오지 못했습니다: 호스트가 이 기기의 파일 다운로드를 허용하지 않습니다',
    ('ClipboardSync', 'Files on the host not copied: over %1 MB or %2 items'):
        '호스트의 파일을 가져오지 못했습니다: %1MB 또는 %2개 항목을 넘었습니다',
    ('Session', 'Turn on clipboard sync to send dropped files to the host'):
        '끌어 놓은 파일을 호스트로 보내려면 클립보드 동기화를 켜세요',

    # Clipboard content that did not move, and permissions per direction
    ('ClipboardSync', "Host files can't be copied: unsupported or duplicate names"):
        '호스트의 파일을 가져올 수 없습니다: 지원하지 않거나 중복된 이름이 있습니다',
    ('ClipboardSync', 'Host files could not be copied'): '호스트의 파일을 가져오지 못했습니다',
    ('ClipboardSync', 'Image on the host not copied: over %1 MB or %2x%2 pixels'):
        '호스트의 이미지를 가져오지 못했습니다: %1MB 또는 %2x%2 픽셀을 넘었습니다',
    ('ClipboardSync', 'Image on the host not copied: it could not be read'):
        '호스트의 이미지를 가져오지 못했습니다: 이미지를 읽을 수 없습니다',
    ('ClipboardSync', 'Image on the host could not be copied'): '호스트의 이미지를 가져오지 못했습니다',
    ('ClipboardSync', 'Text on the host not copied: over %1 MB'): '호스트의 텍스트를 가져오지 못했습니다: %1MB를 넘었습니다',
    ('ClipboardSync', 'Image not sent to the host: over %1 MB or %2x%2 pixels'):
        '이미지를 호스트로 보내지 못했습니다: %1MB 또는 %2x%2 픽셀을 넘었습니다',
    ('ClipboardSync', "Image not sent to the host: too large or in a format that can't be sent"):
        '이미지를 호스트로 보내지 못했습니다: 너무 크거나 보낼 수 없는 형식입니다',
    ('ClipboardSync', 'Image could not be sent to the host'): '이미지를 호스트로 보내지 못했습니다',
    ('ClipboardSync', 'Text not sent to the host: over %1 MB'): '텍스트를 호스트로 보내지 못했습니다: %1MB를 넘었습니다',
    ('ClipboardSync', "Clipboard: the host does not allow reading its clipboard for this device. Allow Clipboard Read in the host's device permissions."):
        '클립보드: 호스트가 이 기기의 호스트 클립보드 읽기를 허용하지 않습니다. 호스트의 기기 권한에서 클립보드 읽기를 허용하세요.',
    ('ClipboardSync', "Clipboard: the host does not allow sending to its clipboard for this device. Allow Clipboard Set in the host's device permissions."):
        '클립보드: 호스트가 이 기기에서 호스트 클립보드로 보내는 것을 허용하지 않습니다. 호스트의 기기 권한에서 클립보드 쓰기를 허용하세요.',
    ('ClipboardSync', "Clipboard: the host does not allow clipboard sync for this device. Allow Clipboard Read and Clipboard Set in the host's device permissions."):
        '클립보드: 호스트가 이 기기의 클립보드 동기화를 허용하지 않습니다. 호스트의 기기 권한에서 클립보드 읽기와 클립보드 쓰기를 허용하세요.',
    ('ClipboardSync', "Files not sent: the host does not allow sending to its clipboard or file upload for this device. Check Clipboard Set and File Upload in the host's device permissions."):
        '파일을 보내지 못했습니다: 호스트가 이 기기의 클립보드 쓰기 또는 파일 업로드를 허용하지 않습니다. 호스트의 기기 권한에서 클립보드 쓰기와 파일 업로드를 확인하세요.',
    ('ClipboardSync', "Files not sent: the host can't take these names"):
        '파일을 보내지 못했습니다: 호스트에서 쓸 수 없는 이름이 있습니다',

    # Keyboard shortcut list (Ctrl+Alt+Shift+H)
    ('Session', 'Keyboard shortcuts'): '단축키',
    ('Session', 'Mouse mode: remote desktop (absolute)'): '마우스 모드: 원격 데스크톱 (절대 좌표)',
    ('Session', 'Mouse mode: game (relative)'): '마우스 모드: 게임 (상대 이동)',
    ('StreamPanel', 'Mouse: remote desktop'): '마우스 모드: 원격 데스크톱',
    ('StreamPanel', 'Handle position'): '손잡이 위치',
    ('StreamPanel', 'HDR'): 'HDR',
    ('PcView', 'Shut down PC…'): 'PC 끄기…',
    ('PcView', 'Restart PC…'): 'PC 다시 시작…',
    ('PcView', 'This PC is no longer in the list.'): '이 PC가 목록에서 사라졌습니다.',
    ('PcView', 'Restart %1'): '%1 다시 시작',
    ('PcView', '%1 restarts in a few seconds. It shows as online again once it has started.'):
        '%1이(가) 몇 초 뒤 다시 시작합니다. 부팅이 끝나면 다시 온라인으로 표시됩니다.',
    ('PcView', 'Restarting the PC…'): 'PC를 다시 시작하는 중…',
    ('PcView', 'Their streams end when the PC restarts.'): 'PC가 다시 시작하면 이 기기들의 스트림이 끊깁니다.',
    ('PcView', 'Restart %1? Apps on it are asked to close first.'): '%1을(를) 다시 시작할까요? 실행 중인 앱에는 먼저 종료를 요청합니다.',
    ('PcView', 'Restart anyway'): '그래도 다시 시작',
    ('PcView', 'Restart'): '다시 시작',
    ('PcView', 'Shut down %1'): '%1 끄기',
    ('PcView', 'This device may not turn the PC off or restart it. In the Shell web UI, open Pairing and allow this device to launch apps.'):
        '이 기기에는 PC를 끄거나 다시 시작할 권한이 없습니다. Shell 웹 UI의 페어링 페이지에서 이 기기에 앱 실행 권한을 주세요.',
    ('PcView', "This host can't be turned off or restarted remotely. Update Shell on the host PC."):
        '이 호스트는 원격으로 끄거나 다시 시작할 수 없습니다. 호스트 PC의 Shell을 업데이트하세요.',
    ('PcView', "Couldn't reach the host: %1"): '호스트에 연결하지 못했습니다: %1',
    ('PcView', '%1 turns off in a few seconds.'): '%1이(가) 몇 초 뒤 꺼집니다.',
    ('PcView', 'Turning the PC off…'): 'PC를 끄는 중…',
    ('PcView', 'Checking who is connected…'): '접속 중인 기기를 확인하는 중…',
    ('PcView', '%1 other device(s) connected to this PC right now'): '지금 이 PC에 다른 기기 %1대가 접속해 있습니다',
    ('PcView', 'Their streams end when the PC turns off.'): 'PC가 꺼지면 이 기기들의 스트림이 끊깁니다.',
    ('PcView', 'Turn %1 off? Apps on it are asked to close first.'): '%1을(를) 끌까요? 실행 중인 앱에는 먼저 종료를 요청합니다.',
    ('PcView', 'Also close apps with unsaved work (it is lost)'): '저장하지 않은 작업이 있는 앱도 닫기 (작업은 사라집니다)',
    ('PcView', 'Shut down anyway'): '그래도 끄기',
    ('PcView', 'Shut down'): '끄기',
    ('PcView', 'Cancel'): '취소',
    ('PcView', 'Close'): '닫기',
    ('StreamPanel', 'Right edge'): '오른쪽',
    ('StreamPanel', 'Left edge'): '왼쪽',
    ('StreamPanel', 'Where the panel handle sits and the panel opens. Press and hold the handle, then drag, to move it up or down.'):
        '패널 손잡이가 놓이고 패널이 열리는 쪽입니다. 손잡이를 길게 누른 채 끌면 위아래로 옮길 수 있습니다.',
    ('StreamPanel', 'Mouse: game'): '마우스 모드: 게임',
    ('StreamPanel', 'Click to switch between game mouse (relative, captured) and remote desktop mouse (absolute, follows the pointer). Ctrl+Alt+Shift+M also switches.'):
        '눌러서 게임 마우스(상대 이동, 커서 잡힘)와 원격 데스크톱 마우스(절대 좌표, 포인터를 그대로 따라감)를 바꿉니다. Ctrl+Alt+Shift+M으로도 바꿀 수 있습니다.',
    ('Session', 'Stream settings'): '스트림 설정',
    ('Session', 'Performance overlay'): '성능 오버레이',
    ('Session', 'Mouse mode (game / remote desktop)'): '마우스 모드 (게임 / 원격 데스크톱)',
    ('Session', 'Full screen'): '전체 화면',
    ('Session', 'Minimize'): '최소화',
    ('Session', 'Release mouse and keyboard'): '마우스·키보드 놓아주기',
    ('Session', 'Keep the pointer in the window'): '마우스를 창 안에 가두기',
    ('Session', 'Send system keys (Win, Alt+Tab)'): '시스템 키 보내기 (Win, Alt+Tab)',
    ('Session', 'Show or hide the cursor (remote desktop mouse)'): '커서 표시/숨김 (원격 데스크톱 마우스)',
    ('Session', 'Type the clipboard text'): '클립보드 텍스트 입력',
    ('Session', 'Cancel a file transfer'): '파일 전송 취소',
    ('Session', 'Disconnect'): '연결 끊기',
    ('Session', 'Quit the host app and close Hermit'): '호스트 앱 종료 후 Hermit 닫기',
    ('Session', 'This list'): '이 목록',
    ('Session', 'Drop files on the window'): '창에 파일 끌어 놓기',
    ('Session', 'host clipboard'): '호스트 클립보드로',
    ('StreamPanel', 'Keyboard shortcuts'): '단축키 보기',
    ('StreamPanel', 'Shows the shortcut list over the stream (also Ctrl+Alt+Shift+H)'):
        '스트림 위에 단축키 목록을 보여줍니다 (Ctrl+Alt+Shift+H로도 열 수 있습니다)',
    ('SettingsView', 'Type the bitrate in Mbps'): '비트레이트를 Mbps 단위로 입력하세요',
    ('SettingsView', 'Recommended for %1x%2 at %3 FPS: %4 Mbps'): '%1x%2 · %3 FPS 권장: %4 Mbps',
    ('SettingsView', 'Recommended for %1x%2 at %3 FPS: %4 Mbps (HEVC/AV1: %5 Mbps)'): '%1x%2 · %3 FPS 권장: %4 Mbps (HEVC/AV1은 %5 Mbps)',

    # Upstream messages that name tools or hosts other than Shell, reworded for Hermit.
    ('PcView', 'Enter the PIN in the Shell web UI on the host PC (https://<host address>:47990).'):
        '호스트 PC의 Shell 웹 UI(https://<호스트 주소>:47990)에서 PIN을 입력하세요.',
    ('SettingsView', 'This unlocks extremely high video bitrates for use with Shell hosts. It should only be used when streaming over an Ethernet LAN connection.'):
        'Shell 호스트에서 쓸 수 있는 매우 높은 비트레이트를 허용합니다. 유선 LAN으로 스트리밍할 때만 쓰세요.',
    ('SettingsView', 'Text you copy locally is sent to the host while streaming, and what you copy on the host is brought back when you switch away from the stream window. With a Shell host, images and files are synced too. Files you copy locally (up to 256 MB) are sent when the stream starts and when you switch back to the stream window. Files copied on the host (up to 4 GB) are offered on the clipboard when you leave the stream window and downloaded while you paste them; they are removed from the clipboard when the stream ends. Older Shell hosts send files up to 256 MB, fetched when you leave the window. Each direction needs its permission on the host: Clipboard Read, Clipboard Set, and File Download or File Upload for files.'):
        '스트리밍 중 이 PC에서 복사한 텍스트를 호스트로 보내고, 호스트에서 복사한 내용은 스트림 창에서 나올 때 가져옵니다. Shell 호스트에서는 이미지와 파일도 동기화합니다. 이 PC에서 복사한 파일(최대 256MB)은 스트림 시작 시와 스트림 창으로 돌아올 때 보냅니다. 호스트에서 복사한 파일(최대 4GB)은 스트림 창에서 나올 때 클립보드에 올려 두고 붙여넣는 동안 내려받으며, 스트림이 끝나면 클립보드에서 지웁니다. 이전 Shell 호스트에서는 256MB까지, 스트림 창에서 나올 때 가져옵니다. 방향마다 호스트의 권한이 필요합니다: 클립보드 읽기, 클립보드 쓰기, 파일은 파일 다운로드와 파일 업로드.',
    ('SettingsView', 'Show a summary after each session'): '세션이 끝나면 요약 보기',
    ('SettingsView', 'After a stream of at least 30 seconds, shows frame rate, network loss and latency compared with your previous sessions. Every session is also added to a history file (CSV) that opens in any spreadsheet app.'):
        '30초 이상 스트리밍한 뒤 프레임 레이트, 네트워크 손실, 지연을 이전 세션들과 비교해 보여줍니다. 모든 세션은 스프레드시트 앱에서 열 수 있는 기록 파일(CSV)에도 저장됩니다.',
    ('SettingsView', 'Open session history folder'): '세션 기록 폴더 열기',
    ('SettingsView', 'Use full-size video packets over the internet (1392 bytes)'): '인터넷 스트리밍에서 큰 비디오 패킷 사용 (1392바이트)',
    ('SettingsView', 'Video packets are normally limited to 1024 bytes when streaming over the internet. Larger packets mean about 26% fewer packets per frame. Turn this off again if the stream breaks up or fails to start: the network path may not carry full-size packets. VPN connections always keep the 1024 byte limit.'):
        '인터넷 스트리밍에서는 보통 비디오 패킷을 1024바이트로 제한합니다. 큰 패킷을 쓰면 프레임당 패킷 수가 약 26% 줄어듭니다. 스트림이 깨지거나 시작되지 않으면 다시 끄세요. 경로가 큰 패킷을 나르지 못하는 것입니다. VPN 연결은 항상 1024바이트를 유지합니다.',
    # Connection stages while starting a stream (Session.cpp stageName, STAGE_* order)
    ('Session', 'platform initialization'): '플랫폼 초기화',
    ('Session', 'name resolution'): '이름 확인',
    ('Session', 'audio stream initialization'): '오디오 스트림 준비',
    ('Session', 'RTSP handshake'): 'RTSP 연결',
    ('Session', 'control stream initialization'): '제어 스트림 준비',
    ('Session', 'video stream initialization'): '비디오 스트림 준비',
    ('Session', 'input stream initialization'): '입력 스트림 준비',
    ('Session', 'control stream establishment'): '제어 스트림 연결',
    ('Session', 'video stream establishment'): '비디오 스트림 연결',
    ('Session', 'audio stream establishment'): '오디오 스트림 연결',
    ('Session', 'input stream establishment'): '입력 스트림 연결',

    # Notices over the stream
    ('Session', 'Slow connection to PC\nReduce your bitrate (Ctrl+Alt+Shift+P)'): 'PC와의 연결이 느립니다\n비트레이트를 낮추세요 (Ctrl+Alt+Shift+P)',
    ('Session', 'Poor connection to PC'): 'PC와의 연결 상태가 나쁩니다',
    ('Session', 'Slow connection · lowering the bitrate automatically'): '연결이 느립니다 · 비트레이트를 자동으로 낮추는 중',
    ('Session', 'Gamepad mouse mode active\nLong press Start to deactivate'): '게임패드 마우스 모드 켜짐\nStart를 길게 눌러 끄기',
    ('Session', 'Changes that need a reconnect (resolution, codec and so on) apply from the next connection'):
        '변경한 해상도·코덱 등은 다음 연결부터 적용됩니다',
    ('Session', 'Mouse and keyboard released. Click the stream to capture them again.'):
        '마우스·키보드를 놓았습니다. 스트림을 클릭하면 다시 잡습니다.',
    ('Session', 'Keep the pointer in the window: on'): '마우스 가두기: 켬',
    ('Session', 'Keep the pointer in the window: off'): '마우스 가두기: 끔',
    ('Session', 'Send system keys: on'): '시스템 키 보내기: 켬',
    ('Session', 'Send system keys: off'): '시스템 키 보내기: 끔',
    ('Session', 'The cursor can only be shown with the remote desktop mouse'): '커서 표시는 원격 데스크톱 마우스에서만 됩니다',
    ('Session', 'There is no text in the clipboard'): '클립보드에 텍스트가 없습니다',
    ('Session', 'There is no file transfer to cancel'): '취소할 파일 전송이 없습니다',
    ('Session', "An attached gamepad has no mapping and won't be usable."): '연결된 게임패드에 매핑이 없어 사용할 수 없습니다.',

    # Stream panel
    ('StreamPanel', 'Sizes from 320x240 to 7680x4320 can be entered.'): '320x240부터 7680x4320까지 입력할 수 있습니다.',
    ('StreamPanel', 'Reconnecting also sets the bitrate to %1 Mbps for the new resolution and frame rate.'):
        '다시 연결하면 비트레이트도 새 해상도·프레임에 맞춰 %1 Mbps가 됩니다.',
    ('StreamPanel', 'Choose metrics'): '지표 선택',
    ('StreamPanel', 'Quit %1? Unsaved progress is lost.'): '%1을(를) 종료할까요? 저장하지 않은 내용은 사라집니다.',
    ('StreamPanel', 'Quit app'): '앱 종료',
    ('StreamPanel', 'Cancel'): '취소',

    # Before the stream: shortcuts worth knowing
    ('StreamSegue', 'Ctrl+Alt+Shift+P stream settings · Ctrl+Alt+Shift+H shortcut list · Ctrl+Alt+Shift+Q disconnect'):
        'Ctrl+Alt+Shift+P 스트림 설정 · Ctrl+Alt+Shift+H 단축키 목록 · Ctrl+Alt+Shift+Q 연결 끊기',
    ('StreamSegue', 'Gamepad: Start+Select+L1+R1 to disconnect'): '게임패드: Start+Select+L1+R1로 연결 끊기',
    ('Session', 'Ctrl+Alt+Shift+P: stream settings · Ctrl+Alt+Shift+Z: release the mouse'):
        'Ctrl+Alt+Shift+P: 스트림 설정 · Ctrl+Alt+Shift+Z: 마우스 놓아주기',

    # Settings
    ('SettingsView', 'A Shell host sets its virtual display to this resolution.'): 'Shell 호스트는 가상 디스플레이를 이 해상도로 맞춥니다.',
    ('SettingsView', 'Streaming conveniences'): '스트리밍 편의 기능',
    ('SettingsView', 'Windowed'): '창 모드',
    ('SettingsView', '%1x%2 (display aspect)'): '%1x%2 (화면 비율)',
    ('StreamPanel', '%1x%2 (display aspect)'): '%1x%2 (화면 비율)',
    ('SettingsView', 'Unlock bitrate limit (Experimental)'): '비트레이트 제한 해제 (실험적)',

    # Upstream sentences that were not true for Hermit and Shell
    ('Session', 'Host software version 3.0 or higher is required for 4K streaming.'): '4K 스트리밍에는 호스트 소프트웨어 3.0 이상이 필요합니다.',
    ('NvHTTP', 'Missing audio capture device. Reinstalling the host software should resolve this error.'):
        '오디오 캡처 장치가 없습니다. 호스트 소프트웨어를 다시 설치하면 이 오류가 해결됩니다.',
    ('main', "This version of Hermit isn't optimized for your PC. Please download the '%1' version of Hermit for the best streaming performance."):
        '이 Hermit 빌드는 이 PC에 맞게 최적화되어 있지 않습니다. 최상의 성능을 위해 %1 빌드를 쓰세요.',
    ('PendingPairingTask', 'The host returned an error: %1'): '호스트가 오류를 반환했습니다: %1',
    ('QPlatformTheme', 'Save'): '저장',
}


def main():
    root = ET.Element('TS', version='2.1', language='ko_KR')
    contexts = {}
    for (context, source), translation in TRANSLATIONS.items():
        if context not in contexts:
            ctx = ET.SubElement(root, 'context')
            ET.SubElement(ctx, 'name').text = context
            contexts[context] = ctx
        message = ET.SubElement(contexts[context], 'message')
        ET.SubElement(message, 'source').text = source
        tr = ET.SubElement(message, 'translation')
        if isinstance(translation, tuple):
            message.set('numerus', 'yes')
            for form in translation:
                ET.SubElement(tr, 'numerusform').text = form
        else:
            tr.text = translation
    ET.indent(root)
    with open(TS, 'w', encoding='utf-8', newline='\n') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n')
        f.write(ET.tostring(root, encoding='unicode'))
        f.write('\n')
    subprocess.run([LRELEASE, TS, '-qm', TS[:-3] + '.qm'], check=True)


if __name__ == '__main__':
    main()
