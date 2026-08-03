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

std::string mcp::Tool::make_tools_list_json(const Tool &tool, const McpRequest &req)
{
	jstream::Writer w;
	w.enable_indent(false);
	w.enable_newline(false);
	w.object({}, [&](){
		w.string("jsonrpc", "2.0");
		mcp_write_id(w, req.id);
		w.object("result", [&](){
			w.array("tools", [&](){
				for (auto const &func : tool.functions()) {
					w.object({}, [&](){
						w.string("name", func.name());
						w.string("description", func.description());
						w.object("inputSchema", [&](){
							w.object("properties", [&](){
								for (auto const &prop : func.input_properties()) {
									w.object(prop.name, [&](){
										w.string("type", prop.type);
									});
								}
							});
							w.array("required", [&](){
								w.string("a");
								w.string("b");
							});
							mcp::Property const &t = func.input_title();
							w.string("title", t.name);
							w.string("type", t.type);
						});
						w.object("outputSchema", [&](){
							w.object("properties", [&](){
								w.object(func.output_property().name, [&](){
									w.string("title", "Result");
									w.string("type", "number");
								});
							});
							w.array("required", [&](){
								w.string("result");
							});
							mcp::Property const &t = func.output_title();
							w.string("title", t.name);
							w.string("type", t.type);
						});
					});
				}
			});
		});
	});
	return w;
}
