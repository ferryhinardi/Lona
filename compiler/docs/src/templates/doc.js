import { graphql } from 'gatsby'
import React from 'react'
import { MDXRenderer } from 'gatsby-mdx'
import Layout from '../components/Layout'

export default function Template({ data, location }) {
  return (
    <Layout location={location}>
      <MDXRenderer>{data.mdx.code.body}</MDXRenderer>
    </Layout>
  )
}

export const query = graphql`
  query DocById($id: String!) {
    mdx(id: { eq: $id }) {
      code {
        body
      }
    }
  }
`