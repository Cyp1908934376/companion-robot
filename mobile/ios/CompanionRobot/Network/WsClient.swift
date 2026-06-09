import Foundation
import Combine

/// WebSocket client for main-brain gateway communication.

enum WsEvent {
    case connected(String)
    case disconnected(String)
    case frameReceived(Data)
    case error(String)
    case reconnecting
}

class WsClient: NSObject, ObservableObject {
    @Published var isConnected = false

    let events = PassthroughSubject<WsEvent, Never>()

    private var webSocketTask: URLSessionWebSocketTask?
    private var session: URLSession!
    private var reconnectAttempt = 0
    private let maxReconnectDelay: TimeInterval = 30
    private var lastUrl: String?
    private var lastToken: String?
    private var reconnectTimer: Timer?

    override init() {
        super.init()
        session = URLSession(configuration: .default, delegate: self, delegateQueue: .main)
    }

    // MARK: - Connection

    func connect(url: String, token: String) {
        lastUrl = url
        lastToken = token

        guard var components = URLComponents(string: url) else {
            events.send(.error("Invalid URL"))
            return
        }

        components.queryItems = [URLQueryItem(name: "token", value: token)]

        guard let wsUrl = components.url else {
            events.send(.error("Failed to build WebSocket URL"))
            return
        }

        webSocketTask = session.webSocketTask(with: wsUrl)
        webSocketTask?.resume()
        startListening()
        startPing()
    }

    func disconnect() {
        reconnectAttempt = 0
        reconnectTimer?.invalidate()
        webSocketTask?.cancel(with: .normalClosure, reason: nil)
        webSocketTask = nil
        isConnected = false
    }

    // MARK: - Send

    func sendBcpFrame(_ data: Data) -> Bool {
        guard !data.isEmpty else { return false }
        webSocketTask?.send(.data(data)) { error in
            if let error = error {
                print("WS send error: \(error)")
            }
        }
        return true
    }

    func sendControlMessage(_ json: String) -> Bool {
        webSocketTask?.send(.string(json)) { error in
            if let error = error {
                print("WS control send error: \(error)")
            }
        }
        return true
    }

    // MARK: - Internal

    private func startListening() {
        webSocketTask?.receive { [weak self] result in
            guard let self = self else { return }

            switch result {
            case .success(.data(let data)):
                self.events.send(.frameReceived(data))
                self.startListening()

            case .success(.string):
                self.startListening()

            case .failure(let error):
                self.events.send(.error(error.localizedDescription))
                self.scheduleReconnect()

            @unknown default:
                break
            }
        }
    }

    private func startPing() {
        Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            self?.webSocketTask?.sendPing { error in
                if let error = error {
                    print("WS ping failed: \(error)")
                }
            }
        }
    }

    private func scheduleReconnect() {
        let delay = min(pow(2.0, Double(reconnectAttempt)), maxReconnectDelay)
        reconnectAttempt += 1

        events.send(.reconnecting)

        reconnectTimer?.invalidate()
        reconnectTimer = Timer.scheduledTimer(withTimeInterval: delay, repeats: false) { [weak self] _ in
            guard let self = self,
                  let url = self.lastUrl,
                  let token = self.lastToken else { return }
            self.connect(url: url, token: token)
        }
    }
}

// MARK: - URLSessionWebSocketDelegate

extension WsClient: URLSessionWebSocketDelegate {
    func urlSession(_ session: URLSession,
                    webSocketTask: URLSessionWebSocketTask,
                    didOpenWithProtocol protocol: String?) {
        isConnected = true
        reconnectAttempt = 0
        events.send(.connected(webSocketTask.currentRequest?.url?.absoluteString ?? ""))
    }

    func urlSession(_ session: URLSession,
                    webSocketTask: URLSessionWebSocketTask,
                    didCloseWith closeCode: URLSessionWebSocketTask.CloseCode,
                    reason: Data?) {
        isConnected = false
        let reasonStr = reason.flatMap { String(data: $0, encoding: .utf8) } ?? ""
        events.send(.disconnected(reasonStr))
    }
}
