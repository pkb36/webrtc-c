import miniupnpc


def main():
    # UPnP 클라이언트 초기화
    upnp = miniupnpc.UPnP()

    # 라우터에 연결
    upnp.discoverdelay = 200
    upnp.discover()

    # 외부 IP 주소 조회
    upnp.selectigd()
    external_ip = upnp.externalipaddress()

    if external_ip:
        print(f"External IP Address: {external_ip}")
    else:
        print("Failed to retrieve external IP address.")
        return


    protocol = 'TCP'
    internal_port = 80  # 내부 포트
    external_port = 12345  # 외부 포트

    result = upnp.addportmapping(external_port, protocol, upnp.lanaddr, internal_port,
                                 "ITECH FileViewer", "")
    if result:
        print(f"Port mapping added successfully: {upnp.lanaddr}:{internal_port} -> {external_ip}:{external_port}")
    else:
        print("Failed to add port mapping")

    with open('local_ip.log', 'w') as file:
        # 파일에 쓰기
        file.write(f'{upnp.lanaddr}\n')
        file.write(f'{external_ip}:{external_port}\n')

if __name__ == "__main__":
    main()