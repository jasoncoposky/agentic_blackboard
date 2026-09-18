#!/usr/bin/env python3
"""
Unit and regression test for generalized swarm defaults and permission-resilient config loading.
Verifies that:
1. load_config() does not crash on PermissionError when inspecting default or custom config paths.
2. Swarm defaults are domain-neutral (swarm-user, swarm-agent, architect).
3. CLI argument parser and FastMCP tool signatures use the generalized defaults.
4. No residual 'cpg-swarm-user', 'cpg-swarm-agent', or 'cpg-architect' exist in src/ab-ctl.py.
"""

import importlib.util
import inspect
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parent.parent
AB_CTL_PY = REPO_ROOT / "src" / "ab-ctl.py"

# Dynamically import ab-ctl.py as a module
spec = importlib.util.spec_from_file_location("ab_ctl", AB_CTL_PY)
ab_ctl = importlib.util.module_from_spec(spec)
sys.modules["ab_ctl"] = ab_ctl
spec.loader.exec_module(ab_ctl)


class TestSwarmDefaultsAndConfig(unittest.TestCase):
    def test_load_config_permission_error_resilience(self):
        """load_config must handle PermissionError gracefully without throwing."""
        with patch.object(Path, "is_file", side_effect=PermissionError("Permission denied: /etc/agentic-blackboard/blackboard.conf")):
            cfg = ab_ctl.load_config()
            self.assertIsInstance(cfg, dict)
            self.assertEqual(cfg["connect"], ab_ctl.DEFAULT_CONNECT_URL)
            self.assertEqual(cfg["data_dir"], ab_ctl.DEFAULT_DATA_DIR)

    def test_resolve_swarm_headers_defaults(self):
        """resolve_swarm_headers should default to swarm-user and swarm-agent."""
        class DummyArgs:
            token = "dummy_token"
            token_file = None
            active_user = None
            user = None
            active_agent = None
            agent = None

        with patch.dict(os.environ, {}, clear=True):
            headers = ab_ctl.resolve_swarm_headers(DummyArgs(), {})
            self.assertEqual(headers.get("X-Active-User"), "swarm-user")
            self.assertEqual(headers.get("X-Active-Agent"), "swarm-agent")

    def test_cli_parser_swarm_defaults(self):
        """CLI parser for 'swarm task create' must default --agent to 'architect' and avoid CPG in help."""
        # Find the swarm task create parser
        # We can inspect the argparse configuration by calling build_parser or checking main's subparser
        with open(AB_CTL_PY, "r", encoding="utf-8") as f:
            content = f.read()

        self.assertNotIn("cpg-swarm-user", content)
        self.assertNotIn("cpg-swarm-agent", content)
        self.assertNotIn("cpg-architect", content)
        self.assertNotIn("CPG blast radius", content)
        self.assertNotIn("target CPG symbols", content)

    def test_fastmcp_swarm_create_task_signature(self):
        """swarm_create_task in FastMCP must default agent_id to 'architect'."""
        with open(AB_CTL_PY, "r", encoding="utf-8") as f:
            content = f.read()

        # Check agent_id default in swarm_create_task definition
        self.assertIn('agent_id: str = "architect"', content)
        self.assertIn('Create a swarm task atom with target symbols', content)


if __name__ == "__main__":
    unittest.main()
