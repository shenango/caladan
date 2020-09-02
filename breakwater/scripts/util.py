import paramiko
from os import system

def execute_remote(conns, cmd, wait=True, must_succeed=True):
    BUF_SIZE = 1024
    sessions = []
    for conn in conns:
        session = conn.get_transport().open_session()
        session.exec_command(cmd)
        sessions.append(session)

    if wait:
        for session in sessions:
            stderr = ""
            if session.recv_exit_status() is not 0 and must_succeed:
                while session.recv_stderr_ready():
                    stderr += session.recv_stderr(BUF_SIZE).decode("utf-8")
                print(stderr)
                exit()

    return sessions

def execute_local(cmd, must_succeed=True):
    if system(cmd) is not 0 and must_succeed:
        exit()
