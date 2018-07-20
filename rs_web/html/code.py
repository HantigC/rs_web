#!/usr/bin/env python
import web

urls = (
	'/','index'
	)

class index:
	def GET(self):
		raise web.seeother('/static/index.html')

app = web.application(urls, globals())

if __name__ == "__main__":
	app.run()
