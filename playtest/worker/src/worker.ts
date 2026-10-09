import { requireMaintainer, requireTester } from './auth'
import { latestBuild, putBuild, putGame, putSources, readSources } from './builds'
import { errorResponse, HttpError } from './http'
import { PAGE } from './page'
import { gameSessions, listSessions, mySessions, readBundle, reviewSession, uploadSession } from './sessions'
import { inviteTester, listTesters, revokeTester } from './testers'

type Route = [method: string, pattern: RegExp, handle: (env: Env, request: Request, url: URL, ...params: string[]) => Promise<Response>]

const ROUTES: Route[] = [
	['GET', /^\/$/, async () => new Response(PAGE, { headers: { 'content-type': 'text/html; charset=utf-8' } })],
	['GET', /^\/api\/games\/([^/]+)\/latest$/, (env, _, __, product) => latestBuild(env, product)],
	['GET', /^\/api\/games\/([^/]+)\/sessions$/, (env, _, __, product) => gameSessions(env, product)],
	['GET', /^\/api\/games\/([^/]+)\/sessions\/([^/]+)\/bundle$/, (env, _, __, product, id) => readBundle(env, id, product)],
	['GET', /^\/api\/testers$/, (env) => listTesters(env)],
	['GET', /^\/api\/me$/, async (env, request) => mySessions(env, await requireTester(env, request))],
	['PUT', /^\/api\/sessions\/([^/]+)$/, async (env, request, _, id) => uploadSession(env, request, await requireTester(env, request), id)],

	['POST', /^\/api\/admin\/testers$/, admin((env, request) => inviteTester(env, request))],
	['DELETE', /^\/api\/admin\/testers\/([^/]+)$/, admin((env, _, __, id) => revokeTester(env, id))],
	['GET', /^\/api\/admin\/sessions$/, admin((env, _, url) => listSessions(env, url))],
	['GET', /^\/api\/admin\/sessions\/([^/]+)\/bundle$/, admin((env, _, __, id) => readBundle(env, id))],
	['POST', /^\/api\/admin\/sessions\/([^/]+)\/review$/, admin((env, request, _, id) => reviewSession(env, request, id))],
	['PUT', /^\/api\/admin\/games\/([^/]+)$/, admin((env, request, _, product) => putGame(env, request, product))],
	['PUT', /^\/api\/admin\/sources\/([^/]+)\/([^/]+)$/, admin((env, request, _, product, build) => putSources(env, request, product, build))],

	['PUT', /^\/api\/builds\/([^/]+)\/([^/]+)$/, maintainer(['admin', 'ci'], (env, request, _, product, build) => putBuild(env, request, product, build))],
	['GET', /^\/api\/sources\/([^/]+)\/([^/]+)$/, maintainer(['admin', 'ci'], (env, _, __, product, build) => readSources(env, product, build))],
]

export default {
	async fetch(request, env): Promise<Response> {
		const url = new URL(request.url)
		try {
			const matches = ROUTES.map(([method, pattern, handle]) => ({ method, match: pattern.exec(url.pathname), handle })).filter((r) => r.match)
			if (!matches.length) throw new HttpError(404, `no route ${url.pathname}`)
			const route = matches.find((r) => r.method === request.method)
			if (!route?.match) throw new HttpError(405, `${request.method} is not allowed on ${url.pathname}`)
			return await route.handle(env, request, url, ...route.match.slice(1).map(decodeURIComponent))
		} catch (error) {
			return errorResponse(error)
		}
	},
} satisfies ExportedHandler<Env>

function admin(handle: Route[2]): Route[2] {
	return maintainer(['admin'], handle)
}

function maintainer(allowed: Parameters<typeof requireMaintainer>[2], handle: Route[2]): Route[2] {
	return async (env, request, url, ...params) => {
		await requireMaintainer(env, request, allowed)
		return handle(env, request, url, ...params)
	}
}
