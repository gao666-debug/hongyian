import argparse
from pathlib import Path
import uvicorn
from backend.app import create_app

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--host',default='127.0.0.1')
    parser.add_argument('--port',type=int,default=8443)
    parser.add_argument('--cert')
    parser.add_argument('--key')
    args=parser.parse_args()
    if bool(args.cert)!=bool(args.key):parser.error('Both --cert and --key are required')
    uvicorn.run(create_app(),host=args.host,port=args.port,ssl_certfile=args.cert,
                ssl_keyfile=args.key,access_log=False)
