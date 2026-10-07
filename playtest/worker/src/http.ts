export const errorResponse = (error: unknown) => {
	if (error instanceof HttpError) return json({ error: error.message }, error.status)
	console.error(JSON.stringify({ message: 'unhandled', error: String(error) }))
	return json({ error: 'internal error' }, 500)
}

export class HttpError extends Error {
	constructor(
		readonly status: number,
		message: string,
	) {
		super(message)
	}
}

export const json = (body: unknown, status = 200) =>
	new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } })

/** The request's JSON body, which must be an object; its fields still need checking by the caller. */
export const readObject = async (request: Request): Promise<Record<string, unknown>> => {
	let body: unknown
	try {
		body = await request.json()
	} catch {
		throw new HttpError(400, 'the body is not JSON')
	}
	if (typeof body !== 'object' || body === null || Array.isArray(body)) throw new HttpError(400, 'the body is not an object')
	return body as Record<string, unknown>
}

/** The declared length of a body R2 will store as it streams in, refused past `limit` bytes. */
export const requireLength = (request: Request, limit: number) => {
	const length = Number(request.headers.get('content-length'))
	if (!request.body || !Number.isInteger(length) || length <= 0) throw new HttpError(411, 'a body with a Content-Length is required')
	if (length > limit) throw new HttpError(413, `the body is over ${limit} bytes`)
	return { body: request.body, length }
}

export const requireName = (value: string | undefined, what: string) => {
	if (!value || !/^[\w.-]{1,64}$/.test(value)) throw new HttpError(400, `${what} must be 1 to 64 letters, digits, '.', '_' or '-'`)
	return value
}
