#!/usr/bin/python3

# import Neutron
import webview
import paws

from backend import server, setWinObj

if __name__ == "__main__":
    w = webview.create_window("Paws", server, width=100, height=100)

    setWinObj(w)

    webview.start()
