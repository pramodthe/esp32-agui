#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace agui {

// Type aliases
using AgentId = std::string;
using ThreadId = std::string;
using RunId = std::string;
using MessageId = std::string;
using ToolCallId = std::string;

// Forward declarations
class Message;
struct Tool;
struct Context;
struct ToolCall;

enum class MessageRole { User, Assistant, System, Tool, Developer, Activity, Reasoning };

struct FunctionCall {
    std::string name;
    std::string arguments;

    FunctionCall() = default;
    FunctionCall(const std::string& n, const std::string& args) : name(n), arguments(args) {}
};

struct ToolCall {
public:
    ToolCallId id;
    std::string callType;
    FunctionCall function;

    ToolCall() : callType("function") {}

    nlohmann::json toJson() const;
    static ToolCall fromJson(const nlohmann::json& j);
};

class Message {
public:
    Message() {}
    Message(const MessageId &mid, const MessageRole &role, const std::string &content);
    ~Message() = default;

    // Role conversion helpers
    static MessageRole roleFromString(const std::string& roleStr);
    static std::string roleToString(MessageRole role);

    // Unified factory — auto-generates ID
    static Message create(MessageRole role, const std::string& content = "",
                          const std::string& name = "",
                          const std::string& toolCallId = "");

    // Unified factory — uses provided ID
    static Message createWithId(const MessageId& id, MessageRole role,
                                const std::string& content = "",
                                const std::string& name = "",
                                const std::string& toolCallId = "");

    const MessageId& id() const { return m_id; }
    MessageRole role() const { return m_role; }
    const std::string& content() const { return m_content; }
    const std::string& name() const { return m_name; }
    const std::vector<ToolCall>& toolCalls() const { return m_toolCalls; }
    const std::string& toolCallId() const { return m_toolCallId; }
    const std::string& activityType() const { return m_activityType; }

    void setRole(const MessageRole &role) {m_role = role;}
    void setContent(const std::string& content) { m_content = content; }
    void setName(const std::string& name) { m_name = name; }
    void appendContent(const std::string& delta) { m_content += delta; }
    void addToolCall(const ToolCall& toolCall) { m_toolCalls.push_back(toolCall); }
    void setActivityType(const std::string& type) { m_activityType = type; }
    void assignEventDelta(const ToolCallId& toolCallId, const std::string &value);
    void appendEventDelta(const ToolCallId& toolCallId, const std::string &delta);

    nlohmann::json toJson() const;
    static Message fromJson(const nlohmann::json& j);

private:
    MessageId m_id;
    MessageRole m_role = MessageRole::User;
    std::string m_content;
    std::string m_name;
    std::vector<ToolCall> m_toolCalls;
    std::string m_toolCallId;
    std::string m_activityType;  // non-empty for MessageRole::Activity messages
};

struct Tool {
    std::string name;
    std::string description;
    nlohmann::json parameters;

    nlohmann::json toJson() const;
    static Tool fromJson(const nlohmann::json& j);
};

struct Context {
    std::string description;
    std::string value;

    nlohmann::json toJson() const;
    static Context fromJson(const nlohmann::json& j);
};

// [device] Human-in-the-loop interrupt — carried by RUN_FINISHED's interrupt outcome. See PATCHES.md.
struct Interrupt {
    std::string id;
    std::string reason;
    std::optional<std::string> message;
    std::optional<ToolCallId> toolCallId;
    std::optional<nlohmann::json> responseSchema;
    std::optional<int64_t> expiresAt;     // ms since epoch
    std::optional<nlohmann::json> metadata;

    nlohmann::json toJson() const;
    static Interrupt fromJson(const nlohmann::json& j);
};

// [device] Resolution of an open interrupt, sent back via RunAgentInput.resume on the next run.
enum class ResumeStatus { Resolved, Cancelled };

struct ResumeEntry {
    std::string interruptId;
    ResumeStatus status = ResumeStatus::Resolved;
    std::optional<nlohmann::json> payload;

    nlohmann::json toJson() const;
    static ResumeEntry fromJson(const nlohmann::json& j);
    static std::string statusToString(ResumeStatus s);
    static ResumeStatus statusFromString(const std::string& s);
};

struct RunAgentInput {
    ThreadId threadId;
    RunId runId;
    std::optional<RunId> parentRunId;
    nlohmann::json state = nlohmann::json::object();
    std::vector<Message> messages;
    std::vector<Tool> tools;
    std::vector<Context> context;
    nlohmann::json forwardedProps;
    std::vector<ResumeEntry> resume;   // [device] non-empty to resume open interrupts

    nlohmann::json toJson() const;
    static RunAgentInput fromJson(const nlohmann::json& j);
};

struct RunAgentResult {
    std::string result;
    std::vector<Message> newMessages;
    nlohmann::json newState = nlohmann::json::object();

    RunAgentResult() = default;
};

// Forward declarations
class IAgentSubscriber;

class RunAgentParams {
public:
    RunAgentParams() = default;

    ThreadId threadId;
    RunId runId;
    std::optional<RunId> parentRunId;
    std::vector<Tool> tools;
    std::vector<Context> context;
    nlohmann::json forwardedProps;
    std::vector<Message> messages;
    nlohmann::json state;  // null by default — null means "not set, use EventHandler state"
    std::vector<std::shared_ptr<IAgentSubscriber>> subscribers;
    std::vector<ResumeEntry> resume;   // [device] interrupt resolutions for a resumed run

    RunAgentParams& withRunId(const RunId& id);
    RunAgentParams& withParentRunId(const RunId& id);
    RunAgentParams& addTool(const Tool& tool);
    RunAgentParams& addContext(const Context& ctx);
    RunAgentParams& withForwardedProps(const nlohmann::json& props);
    RunAgentParams& withState(const nlohmann::json& s);
    RunAgentParams& addMessage(const Message& msg);
    RunAgentParams& addUserMessage(const std::string& content);
    RunAgentParams& addSubscriber(std::shared_ptr<IAgentSubscriber> subscriber);
    RunAgentParams& addResume(const ResumeEntry& entry);   // [device]
};

}  // namespace agui
