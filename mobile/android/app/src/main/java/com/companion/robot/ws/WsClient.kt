package com.companion.robot.ws

import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import okhttp3.*
import java.util.concurrent.TimeUnit

/**
 * WebSocket client for main-brain gateway communication.
 *
 * Responsibilities:
 *   - Connect to gateway (ws:// or wss://)
 *   - Forward BCP frames: robot ←BLE→ phone ←WS→ gateway
 *   - Handle reconnection with exponential backoff
 *   - Heartbeat (WebSocket Ping every 30s)
 */

sealed class WsEvent {
    data class Connected(val url: String) : WsEvent()
    data class Disconnected(val reason: String) : WsEvent()
    data class FrameReceived(val data: ByteArray) : WsEvent()
    data class Error(val message: String) : WsEvent()
    object Reconnecting : WsEvent()
}

class WsClient {

    private val _events = MutableSharedFlow<WsEvent>(replay = 0)
    val events: SharedFlow<WsEvent> = _events

    private var webSocket: WebSocket? = null
    private var reconnectAttempt = 0
    private val maxReconnectDelay = 30_000L // 30 seconds max

    private val client = OkHttpClient.Builder()
        .pingInterval(30, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS) // no read timeout for WS
        .build()

    /**
     * Connect to gateway WebSocket.
     */
    fun connect(url: String, token: String) {
        val request = Request.Builder()
            .url("$url/ws?token=$token")
            .build()

        webSocket = client.newWebSocket(request, wsListener)
    }

    /**
     * Send a BCP frame over WebSocket (binary frame).
     */
    fun sendBcpFrame(data: ByteArray): Boolean {
        if (data.isEmpty()) return false
        return webSocket?.send(ByteString.of(*data)) ?: false
    }

    /**
     * Send a JSON control message (text frame).
     */
    fun sendControlMessage(json: String): Boolean {
        return webSocket?.send(json) ?: false
    }

    fun disconnect() {
        webSocket?.close(1000, "User disconnected")
        webSocket = null
        reconnectAttempt = 0
    }

    // ── WebSocket listener ─────────────────────────────────────

    private val wsListener = object : WebSocketListener() {
        override fun onOpen(webSocket: WebSocket, response: Response) {
            reconnectAttempt = 0
            _events.tryEmit(WsEvent.Connected(response.request.url.toString()))
        }

        override fun onMessage(webSocket: WebSocket, text: String) {
            // JSON control message
            // In production: parse and handle connection state/errors
        }

        override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
            // Binary BCP frame
            _events.tryEmit(WsEvent.FrameReceived(bytes.toByteArray()))
        }

        override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
            webSocket.close(1000, null)
            _events.tryEmit(WsEvent.Disconnected(reason))
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
            _events.tryEmit(WsEvent.Error(t.message ?: "WebSocket error"))
            scheduleReconnect()
        }
    }

    /**
     * Exponential backoff reconnection: 1s → 2s → 4s → ... → 30s max.
     */
    private fun scheduleReconnect() {
        val delay = minOf(1_000L shl reconnectAttempt, maxReconnectDelay)
        reconnectAttempt++

        _events.tryEmit(WsEvent.Reconnecting)

        // In production: use coroutine delay to reconnect
        // viewModelScope.launch {
        //     delay(delay)
        //     connect(lastUrl, lastToken)
        // }
    }
}
