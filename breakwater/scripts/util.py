import paramiko

def execute_command(conns, cmd, wait=True):
    sessions = []
    for conn in conns:
        session = conn.get_transport().open_session()
        session.exec_command(cmd)
        sessions.append(session)

    if wait:
        for session in sessions:
            session.recv_exit_status()

    return sessions
