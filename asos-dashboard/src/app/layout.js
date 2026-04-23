import "./globals.css";
import Sidebar from "../components/Sidebar";


export const metadata = {
  title: "ASOS Agentic Dashboard",
  description: "Distributed Knowledge Swarm Governance",
};

export default function RootLayout({ children }) {
  return (
    <html lang="en">
      <body>
        <div className="layout-container">
          <Sidebar />
          
          <div className="main-wrapper">

            <header className="top-bar glass">
              <div className="breadcrumb">Dashboard / Overview</div>
              <div className="user-profile">
                <span className="agent-id">Agent v0.4-α</span>
                <div className="avatar"></div>
              </div>
            </header>
            
            <main className="content">
              {children}
            </main>
          </div>
        </div>

      </body>
    </html>
  );
}

