#include "mcp.h"

#include <misc/jstream.h>

// --- MCP (/mcp) JSON-RPC request/response helpers ---

void mcp::mcp_write_id(jstream::Writer &w, const McpId &id)
{
	switch (id.kind) {
	case mcp::McpId::Kind::String:
		w.string("id", id.str);
		break;
	case mcp::McpId::Kind::Null:
		w.symbol("id", jstream::Null);
		break;
	case mcp::McpId::Kind::Number:
	case mcp::McpId::Kind::Missing:
	default:
		w.number("id", id.number);
		break;
	}
}

std::string mcp::Tools::make_tools_list_json(const Tools &tool, const McpRequest &req)
{
	jstream::Writer w;
	w.enable_indent(false);
	w.enable_newline(false);
	w.object({}, [&](){
		w.string("jsonrpc", "2.0");
		mcp_write_id(w, req.id);
		w.object("result", [&](){
			w.array("tools", [&](){
				for (AbstractTool *func : tool.functions()) {
					ToolSchema const &schema = func->schema();
					w.object({}, [&](){
						w.string("name", schema.name);
						w.string("description", schema.description);
						w.object("inputSchema", [&](){
							w.object("properties", [&](){
								for (auto const &prop : schema.input_properties) {
									w.object(prop.name, [&](){
										w.string("type", prop.type);
									});
								}
							});
							w.array("required", [&](){
								for (auto const &prop : schema.input_properties) {
									w.string(prop.name);
								}
							});
							w.string("title", "Input");
							w.string("type", "object");
						});
						w.object("outputSchema", [&](){
							w.object("properties", [&](){
								w.object(schema.output_property.name, [&](){
									w.string("title", schema.output_property.title);
									w.string("type", schema.output_property.type);
								});
							});
							w.array("required", [&](){
								w.string(schema.output_property.name);
							});
							w.string("title", "Output");
							w.string("type", "object");
						});
					});
				}
			});
		});
	});
	return w;
}

void mcp::Tools::install_mcp_tool(const std::shared_ptr<AbstractTool> &tool)
{
	std::string name = tool->schema().name;
	functions_[name] = tool;
}
