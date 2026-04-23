"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";

export default function Sidebar() {
  const pathname = usePathname();

  const links = [
    { href: "/", label: "Overview" },
    { href: "/graph", label: "Librarian" },
    { href: "/governance", label: "Governance" },
    { href: "/settings", label: "Substrate" },
  ];

  return (
    <aside className="sidebar glass">
      <div className="logo">
        <span className="logo-icon pulse"></span>
        <h2 className="logo-text">ASOS</h2>
      </div>
      <nav className="nav-links">
        {links.map((link) => (
          <Link
            key={link.href}
            href={link.href}
            className={`nav-item ${pathname === link.href ? "active" : ""}`}
          >
            {link.label}
          </Link>
        ))}
      </nav>
      <div className="sidebar-footer">
        <div className="status-badge">
          <span className="status-dot online"></span>
          Swarm Stable
        </div>
      </div>
    </aside>
  );
}
