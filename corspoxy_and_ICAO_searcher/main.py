from app.create_app import create_app

app = create_app()

if __name__ == "__main__":
    import uvicorn

    server = uvicorn.Server(uvicorn.Config(app, host="0.0.0.0", port=8002))
    server.run()
