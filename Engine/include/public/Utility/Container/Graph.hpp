#ifndef _GRAPH_H_
#define _GRAPH_H_

namespace Sleak
{
    /**
 * @class Graph
 * @brief Represents a graph data structure with vertices and edges.
 *
 * The Graph class provides functionality for managing a collection of vertices and their
 * connections (edges). It is suitable for modeling networks, relationships, and
 * interconnected systems. Use this class when you need to represent entities with
 * connections, such as social networks, road networks, or dependency graphs.
 *
 * Example Use Cases:
 * - Representing a social network where vertices are users and edges are friendships.
 * - Modeling a road network for navigation and route planning.
 * - Implementing dependency tracking in a build system.
 * - Simulating network communication protocols.
 *
 * Implementation Details:
 * - Uses an adjacency list representation for efficient storage of sparse graphs.
 * - Provides methods for adding vertices and edges, and for traversing the graph.
 */
    template <typename T>
    class Graph
    {
    private:
        struct Node
        {
            T data;
            Node* next;

            Node(const T& value) : data(value), next(nullptr) {}
        };

        struct Vertex
        {
            T data;
            Node* adjacencyList;
            Vertex* next;
        };

        Vertex* vertices;

        Vertex* findVertex(const T& value)
        {
            Vertex* current = vertices;
            while (current)
            {
                if (current->data == value)
                    return current;
                current = current->next;
            }
            return nullptr;
        }

    public:
        Graph() : vertices(nullptr) {}

        ~Graph()
        {
            while (vertices)
            {
                Vertex* temp = vertices;
                while (temp->adjacencyList)
                {
                    Node* adjTemp = temp->adjacencyList;
                    temp->adjacencyList = temp->adjacencyList->next;
                    delete adjTemp;
                }
                vertices = vertices->next;
                delete temp;
            }
        }

        void addVertex(const T& value)
        {
            if (!findVertex(value))
            {
                Vertex* newVertex = new Vertex{value, nullptr, vertices};
                vertices = newVertex;
            }
        }

        void addEdge(const T& from, const T& to)
        {
            Vertex* fromVertex = findVertex(from);
            Vertex* toVertex = findVertex(to);

            if (!fromVertex || !toVertex)
                return;

            Node* newEdge = new Node(to);
            newEdge->next = fromVertex->adjacencyList;
            fromVertex->adjacencyList = newEdge;
        }
    };
}

#endif // _GRAPH_H_
