(() => {
  'use strict';

  const BUTTON_ATTRIBUTE = 'data-vlc-spanning-button';
  const BUTTON_LABEL = '⚡ Span';
  const PROTOCOL_SCHEME = 'vlc-spanning:';
  let feedbackTimer;
  let observerScheduled = false;

  function getToolbar() {
    return document.querySelector('.html5-video-player .ytp-right-controls');
  }

  function getVideo() {
    return document.querySelector('video.html5-main-video, video');
  }

  function getCanonicalVideoUrl() {
    const canonicalLink = document.querySelector('link[rel="canonical"]');
    const candidate = canonicalLink?.href || window.location.href;

    try {
      const url = new URL(candidate);
      const validHost = url.protocol === 'https:' &&
        (url.hostname === 'www.youtube.com' || url.hostname === 'youtube.com');
      const validPath = url.pathname === '/watch' || url.pathname.startsWith('/shorts/');

      if (!validHost || !validPath) {
        return null;
      }

      if (url.pathname === '/watch' && !url.searchParams.get('v')) {
        return null;
      }

      return url.toString();
    } catch {
      return null;
    }
  }

  function setButtonState(button, state, message) {
    window.clearTimeout(feedbackTimer);
    button.dataset.state = state;
    button.textContent = state === 'success' ? '✓ Sent' : state === 'error' ? '! Error' : BUTTON_LABEL;
    button.style.color = state === 'success' ? '#7ee787' : state === 'error' ? '#ff8a8a' : '';
    button.title = message;
    button.setAttribute('aria-label', message);

    if (state !== 'ready') {
      feedbackTimer = window.setTimeout(() => {
        if (button.isConnected) {
          button.dataset.state = 'ready';
          button.title = 'Send this video to VLC Spanning Player';
          button.setAttribute('aria-label', 'Send this video to VLC Spanning Player');
        }
      }, 2200);
    }
  }

  function dispatchToVlc(button) {
    const video = getVideo();
    if (!video) {
      setButtonState(button, 'error', 'No active video found');
      return;
    }

    const videoUrl = getCanonicalVideoUrl();
    if (!videoUrl) {
      setButtonState(button, 'error', 'The current page is not a valid YouTube video');
      return;
    }

    const timestamp = Number.isFinite(video.currentTime) && video.currentTime >= 0
      ? video.currentTime.toFixed(3)
      : '0.000';
    const protocolUrl = `${PROTOCOL_SCHEME}//play?url=${encodeURIComponent(videoUrl)}&t=${timestamp}`;

    video.pause();

    const launcher = document.createElement('a');
    launcher.href = protocolUrl;
    launcher.tabIndex = -1;
    launcher.setAttribute('aria-hidden', 'true');
    launcher.style.display = 'none';
    document.body.appendChild(launcher);
    launcher.click();
    launcher.remove();

    setButtonState(button, 'success', 'Sent to VLC Spanning Player');
  }

  function createButton() {
    const button = document.createElement('button');
    button.className = 'ytp-button';
    button.type = 'button';
    button.setAttribute(BUTTON_ATTRIBUTE, '');
    button.textContent = BUTTON_LABEL;
    button.title = 'Send this video to VLC Spanning Player';
    button.setAttribute('aria-label', 'Send this video to VLC Spanning Player');
    button.dataset.state = 'ready';
    button.style.width = '72px';
    button.style.fontSize = '13px';
    button.style.fontWeight = '600';
    button.style.whiteSpace = 'nowrap';
    button.addEventListener('click', (event) => {
      event.preventDefault();
      event.stopPropagation();
      dispatchToVlc(button);
    });
    return button;
  }

  function ensureButton() {
    const toolbar = getToolbar();
    if (!toolbar) {
      return;
    }

    let button = toolbar.querySelector(`[${BUTTON_ATTRIBUTE}]`);
    if (!button) {
      button = createButton();
    }

    const fullscreenButton = toolbar.querySelector('.ytp-fullscreen-button');
    const desiredPosition = fullscreenButton || null;
    if (button.parentElement !== toolbar || button.nextElementSibling !== desiredPosition) {
      toolbar.insertBefore(button, desiredPosition);
    }
  }

  function scheduleEnsureButton() {
    if (observerScheduled) {
      return;
    }

    observerScheduled = true;
    window.requestAnimationFrame(() => {
      observerScheduled = false;
      ensureButton();
    });
  }

  const observer = new MutationObserver(scheduleEnsureButton);
  observer.observe(document.documentElement, { childList: true, subtree: true });
  scheduleEnsureButton();
})();
