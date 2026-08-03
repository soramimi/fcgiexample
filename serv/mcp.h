#ifndef MCP_H
#define MCP_H

#include <string>
#include <vector>
#include <optional>
#include <misc/toi.h>
#include <misc/fmt.h>
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
	std::string type;
	Property() = default;
	Property(std::string const &name, std::string const &type)
		: name(name)
		, type(type)
	{
	}
};
class AbstractFunction {
public:
	virtual ~AbstractFunction() = default;
	virtual std::string name() const = 0;
	virtual std::string description() const = 0;
	virtual Property input_title() const = 0;
	virtual Property output_title() const = 0;
	virtual std::vector<Property> const &input_properties() const = 0;
	virtual Property const &output_property() const = 0;
};
class Function : public AbstractFunction {
private:
	std::string name_;
	std::string description_;
	Property input_title_;
	Property output_title_;
	std::vector<Property> input_properties_;
	Property output_property_;
public:
	Function()
		: name_("add")
		, description_("Add two numbers")
	{
		input_title_ = Property("addArguments", "object");
		output_title_ = Property("addOutput", "object");
		input_properties_.emplace_back("a", "number");
		input_properties_.emplace_back("b", "number");
		output_property_ = Property("result", "number");
	}
	std::string name() const override
	{
		return name_;
	}
	std::string description() const override
	{
		return description_;
	}
	Property input_title() const override
	{
		return input_title_;
	}
	Property output_title() const override
	{
		return output_title_;
	}
	std::vector<Property> const &input_properties() const override
	{
		return input_properties_;
	}
	Property const &output_property() const override
	{
		return output_property_;
	}
	std::optional<std::string> call(std::shared_ptr<void> context, std::vector<std::string> const &args) const
	{
		if (args.size() != 2) return std::nullopt;
		int a = toi<int>(args[0]);
		int b = toi<int>(args[1]);
		int ans = a + b;
		return fmt("%d")(ans);
	}
};

class Tool {
private:
	std::vector<Function> functions_;
	static std::string make_tools_list_json(mcp::Tool const &tool, McpRequest const &req);
public:
	Tool()
	{
		functions_.emplace_back();
	}
	std::vector<Function> const &functions() const
	{
		return functions_;
	}
	std::optional<Function> find_function(std::string const &name) const
	{
		for (const auto &func : functions_) {
			if (func.name() == name) {
				return func;
			}
		}
		return std::nullopt;
	}
	std::string tools_list_json(McpRequest const &req)
	{
		return make_tools_list_json(*this, req);
	}
};

void mcp_write_id(jstream::Writer &w, McpId const &id);

} // namespace mcp

#endif // MCP_H
