#ifndef MCP_H
#define MCP_H

#include <string>
#include <vector>
#include <optional>
#include <misc/toi.h>
#include <misc/fmt.h>
#include <map>
#include <memory>

namespace jstream { class Writer; }

namespace mcp {

struct McpId {
	enum class Kind { Missing, Number, String, Null };
	Kind kind = Kind::Missing;
	double number = 0;
	std::string str;
};

struct McpRequest {
	std::string method;
	struct Params {
		std::string protocolVersion;
		struct Capabilities {
			struct Experimental {
			};
			struct Prompts {
				bool listChanged = false;
			};
			struct Resources {
				bool subscribe = false;
				bool listChanged = false;
			};
			struct Tools {
				bool listChanged = false;
			};
			Experimental experimental;
			Prompts prompts;
			Resources resources;
			Tools tools;
		};
		struct ServerInfo {
			std::string name;
			std::string version;
		};
		struct ClientInfo {
			std::string name;
			std::string version;
		};
		Capabilities capabilities;
		ServerInfo serverInfo;
		ClientInfo clientInfo;
		struct Argument {
			std::string name;
			std::string value;
		};
		std::string name;
		std::vector<Argument> arguments;
	};
	Params params;
	std::string jsonrpc;
	mcp::McpId id;

	// Looks up a tool-call argument by name; nullptr if absent (order in the
	// request must not matter, only the declared parameter names do).
	Params::Argument const *find_argument(std::string const &name) const
	{
		for (Params::Argument const &arg : params.arguments) {
			if (arg.name == name) {
				return &arg;
			}
		}
		return nullptr;
	}
};

struct Property {
	std::string name;
	std::string title;
	std::string type;
	Property() = default;
	Property(std::string const &name, std::string title, std::string const &type)
		: name(name)
		, title(title)
		, type(type)
	{
	}
};

struct ToolSchema {
	std::string name;
	std::string description;
	std::vector<Property> input_properties;
	Property output_property;
};

class AbstractTool {
private:
	ToolSchema schema_;
protected:
public:
	ToolSchema &schema()
	{
		return schema_;
	}
	ToolSchema const &schema() const
	{
		return schema_;
	}
public:
	virtual ~AbstractTool() = default;
	AbstractTool(std::string const &name, std::string const &description)
	{
		schema_.name = name;
		schema_.description = description;
		schema_.output_property = Property("result", "Result", "string");
	}
	void add_argument(std::string name, std::string title, std::string type)
	{
		schema_.input_properties.emplace_back(name, title, type);
	}
	virtual std::optional<std::string> call(std::shared_ptr<void> context, std::vector<std::string> const &args) const = 0;
};

class Tools {
private:
	std::map<std::string, std::shared_ptr<AbstractTool>> functions_;
	
	static std::string make_tools_list_json(mcp::Tools const &tool, McpRequest const &req);
public:
	std::vector<AbstractTool *> functions() const
	{
		std::vector<AbstractTool *> result;
		for (const auto &pair : functions_) {
			result.push_back(pair.second.get());
		}
		return result;
	}
	std::shared_ptr<AbstractTool> find_function(std::string const &name) const
	{
		auto it = functions_.find(name);
		if (it != functions_.end()) {
			return it->second;
		}
		return nullptr;
	}
	std::string tools_list_json(McpRequest const &req) const
	{
		return make_tools_list_json(*this, req);
	}
	
	void install_mcp_tool(std::shared_ptr<AbstractTool> const &tool);	
	template <typename T, typename... Args> void emplace_tool(Args&&...args)
	{
		install_mcp_tool(std::make_shared<T>(std::forward<Args>(args)...));
	}
};

void mcp_write_id(jstream::Writer &w, McpId const &id);

} // namespace mcp

#endif // MCP_H
