import SwiftUI

struct DialogueView: View {
    @EnvironmentObject var viewModel: RobotViewModel
    let deviceId: String

    @State private var inputText = ""
    @State private var isRecording = false

    var body: some View {
        VStack(spacing: 0) {
            if viewModel.uiState.dialogueHistory.isEmpty {
                VStack(spacing: 16) {
                    Image(systemName: "bubble.left.and.bubble.right")
                        .font(.system(size: 48))
                        .foregroundColor(.secondary)
                    Text("Start a conversation with your robot!")
                        .font(.body)
                        .foregroundColor(.secondary)
                    Text("Try: \"Hello\", \"Move forward\", \"Tell me a joke\"")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(spacing: 8) {
                            ForEach(viewModel.uiState.dialogueHistory) { entry in
                                DialogueBubble(entry: entry)
                                    .id(entry.id)
                            }
                        }
                        .padding()
                    }
                    .onChange(of: viewModel.uiState.dialogueHistory.count) { _ in
                        if let last = viewModel.uiState.dialogueHistory.last {
                            withAnimation {
                                proxy.scrollTo(last.id, anchor: .bottom)
                            }
                        }
                    }
                }
            }

            // Input bar
            HStack(spacing: 8) {
                Button(action: { isRecording.toggle() }) {
                    Image(systemName: isRecording ? "stop.circle.fill" : "mic.circle.fill")
                        .font(.title2)
                        .foregroundColor(isRecording ? .red : .accentColor)
                }

                TextField("Type a message...", text: $inputText, axis: .vertical)
                    .textFieldStyle(.roundedBorder)
                    .lineLimit(3)

                Button(action: {
                    guard !inputText.trimmingCharacters(in: .whitespaces).isEmpty else { return }
                    viewModel.sendSpeakCommand(inputText.trimmingCharacters(in: .whitespaces))
                    inputText = ""
                }) {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.title2)
                        .foregroundColor(.accentColor)
                }
                .disabled(inputText.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            .padding()
            .background(Color(.systemBackground))
            .overlay(Divider(), alignment: .top)
        }
        .navigationTitle("Dialogue")
    }
}

struct DialogueBubble: View {
    let entry: DialogueEntry

    var isUser: Bool { entry.role == "user" }

    var body: some View {
        HStack {
            if isUser { Spacer() }

            VStack(alignment: isUser ? .trailing : .leading, spacing: 4) {
                Text(isUser ? "You" : "Robot")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                    .fontWeight(.bold)

                Text(entry.text)
                    .font(.body)
                    .padding(12)
                    .background(isUser ? Color.accentColor.opacity(0.2) : Color(.systemGray5))
                    .cornerRadius(16)
            }
            .frame(maxWidth: 300, alignment: isUser ? .trailing : .leading)

            if !isUser { Spacer() }
        }
    }
}
