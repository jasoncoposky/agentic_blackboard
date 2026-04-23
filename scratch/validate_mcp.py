import asyncio
import json
from asos_mcp_server import mcp

async def main():
    print("--- ASOS MCP Integration Test ---")
    
    try:
        # 1. Test ensure_node
        print("\n[Tool] Calling ensure_node...")
        res = await mcp.call_tool("ensure_node", {
            "type": "IDENTITY", 
            "id": "mcp_verifier", 
            "description": "MCP Automated Verifier",
            "status": "ACTIVE"
        })
        print(f"Response: {res}")

        # 2. Test commit_knowledge_bundle
        print("\n[Tool] Calling commit_knowledge_bundle...")
        atoms = [
            {
                "statement": "ASOS MCP Integration verified successfully.", 
                "tags": ["MCP", "VERIFIED"],
                "content": "This atom was pushed through the MCP bridge layer."
            }
        ]
        res = await mcp.call_tool("commit_knowledge_bundle", {
            "project_id": "project:governance",
            "agent_id": "mcp_verifier",
            "atoms": atoms
        })
        print(f"Response: {res}")

        # 3. Test get_resource
        print("\n[Resource] Reading asos://schema...")
        res = await mcp.read_resource("asos://schema")
        # Resource returns content objects, we want the text
        print(f"Response (truncated): {res[:200]}...")

        # 4. Test query_substrate
        print("\n[Tool] Querying for the verification atom...")
        res = await mcp.call_tool("query_substrate", {
            "where_eq": {"id": "mcp_verifier"}
        })
        print(f"Response: {res}")

        print("\n--- MCP Validation Complete ---")
        
    except Exception as e:
        print(f"\nMCP Test Failed: {e}")

if __name__ == "__main__":
    asyncio.run(main())
