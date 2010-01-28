"""Aliases for qbuf.*_support.
"""

import sys
from qbuf import socket_support as socket
sys.modules['qbuf.support.socket'] = socket
from qbuf import twisted_support as twisted
sys.modules['qbuf.support.twisted'] = twisted
