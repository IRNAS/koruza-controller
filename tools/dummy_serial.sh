#!/bin/bash
echo ">>> Creating pair of dummy serial devices (test0, test1)..."
echo ">>> Connect koruza-control to test0 and interact with test1."
socat PTY,link=test0 PTY,link=test1
